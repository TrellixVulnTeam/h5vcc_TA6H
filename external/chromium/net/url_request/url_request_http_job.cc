// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/url_request_http_job.h"

#include "base/base_switches.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/file_util.h"
#include "base/file_version_info.h"
#include "base/message_loop.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram.h"
#include "base/rand_util.h"
#include "base/string_util.h"
#include "base/time.h"
#include "net/base/cert_status_flags.h"
#include "net/base/filter.h"
#include "net/base/host_port_pair.h"
#include "net/base/load_flags.h"
#include "net/base/mime_util.h"
#include "net/base/net_errors.h"
#include "net/base/net_util.h"
#include "net/base/network_delegate.h"
#if !defined(__LB_SHELL__)
#include "net/base/sdch_manager.h"
#endif
#include "net/base/ssl_cert_request_info.h"
#include "net/base/ssl_config_service.h"
#include "net/cookies/cookie_monster.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/http/http_status_code.h"
#include "net/http/http_transaction.h"
#include "net/http/http_transaction_delegate.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/http_util.h"
#include "net/url_request/fraudulent_certificate_reporter.h"
#include "net/url_request/http_user_agent_settings.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_error_job.h"
#include "net/url_request/url_request_redirect_job.h"
#include "net/url_request/url_request_throttler_header_adapter.h"
#include "net/url_request/url_request_throttler_manager.h"

static const char kAvailDictionaryHeader[] = "Avail-Dictionary";

namespace net {

class URLRequestHttpJob::HttpFilterContext : public FilterContext {
 public:
  explicit HttpFilterContext(URLRequestHttpJob* job);
  virtual ~HttpFilterContext();

  // FilterContext implementation.
  virtual bool GetMimeType(std::string* mime_type) const OVERRIDE;
  virtual bool GetURL(GURL* gurl) const OVERRIDE;
  virtual base::Time GetRequestTime() const OVERRIDE;
  virtual bool IsCachedContent() const OVERRIDE;
  virtual bool IsDownload() const OVERRIDE;
  virtual bool IsSdchResponse() const OVERRIDE;
  virtual int64 GetByteReadCount() const OVERRIDE;
  virtual int GetResponseCode() const OVERRIDE;
  virtual void RecordPacketStats(StatisticSelector statistic) const OVERRIDE;

  // Method to allow us to reset filter context for a response that should have
  // been SDCH encoded when there is an update due to an explicit HTTP header.
  void ResetSdchResponseToFalse();

 private:
  URLRequestHttpJob* job_;

  DISALLOW_COPY_AND_ASSIGN(HttpFilterContext);
};

class URLRequestHttpJob::HttpTransactionDelegateImpl
    : public HttpTransactionDelegate {
 public:
  explicit HttpTransactionDelegateImpl(URLRequest* request)
      : request_(request),
        network_delegate_(request->context()->network_delegate()),
        cache_active_(false),
        network_active_(false) {
  }
  virtual ~HttpTransactionDelegateImpl() {
    OnDetachRequest();
  }
  void OnDetachRequest() {
    if (request_ == NULL || network_delegate_ == NULL)
      return;
    network_delegate_->NotifyRequestWaitStateChange(
        *request_,
        NetworkDelegate::REQUEST_WAIT_STATE_RESET);
    cache_active_ = false;
    network_active_ = false;
    request_ = NULL;
  }
  virtual void OnCacheActionStart() OVERRIDE {
    if (request_ == NULL || network_delegate_ == NULL)
      return;
    DCHECK(!cache_active_ && !network_active_);
    cache_active_ = true;
    network_delegate_->NotifyRequestWaitStateChange(
        *request_,
        NetworkDelegate::REQUEST_WAIT_STATE_CACHE_START);
  }
  virtual void OnCacheActionFinish() OVERRIDE {
    if (request_ == NULL || network_delegate_ == NULL)
      return;
    DCHECK(cache_active_ && !network_active_);
    cache_active_ = false;
    network_delegate_->NotifyRequestWaitStateChange(
        *request_,
        NetworkDelegate::REQUEST_WAIT_STATE_CACHE_FINISH);
  }
  virtual void OnNetworkActionStart() OVERRIDE {
    if (request_ == NULL || network_delegate_ == NULL)
      return;
    DCHECK(!cache_active_ && !network_active_);
    network_active_ = true;
    network_delegate_->NotifyRequestWaitStateChange(
        *request_,
        NetworkDelegate::REQUEST_WAIT_STATE_NETWORK_START);
  }
  virtual void OnNetworkActionFinish() OVERRIDE {
    if (request_ == NULL || network_delegate_ == NULL)
      return;
    DCHECK(!cache_active_ && network_active_);
    network_active_ = false;
    network_delegate_->NotifyRequestWaitStateChange(
        *request_,
        NetworkDelegate::REQUEST_WAIT_STATE_NETWORK_FINISH);
  }
 private:
  URLRequest* request_;
  NetworkDelegate* network_delegate_;
  bool cache_active_;
  bool network_active_;
};

URLRequestHttpJob::HttpFilterContext::HttpFilterContext(URLRequestHttpJob* job)
    : job_(job) {
  DCHECK(job_);
}

URLRequestHttpJob::HttpFilterContext::~HttpFilterContext() {
}

bool URLRequestHttpJob::HttpFilterContext::GetMimeType(
    std::string* mime_type) const {
  return job_->GetMimeType(mime_type);
}

bool URLRequestHttpJob::HttpFilterContext::GetURL(GURL* gurl) const {
  if (!job_->request())
    return false;
  *gurl = job_->request()->url();
  return true;
}

base::Time URLRequestHttpJob::HttpFilterContext::GetRequestTime() const {
  return job_->request() ? job_->request()->request_time() : base::Time();
}

bool URLRequestHttpJob::HttpFilterContext::IsCachedContent() const {
  return job_->is_cached_content_;
}

bool URLRequestHttpJob::HttpFilterContext::IsDownload() const {
  return (job_->request_info_.load_flags & LOAD_IS_DOWNLOAD) != 0;
}

void URLRequestHttpJob::HttpFilterContext::ResetSdchResponseToFalse() {
  DCHECK(job_->sdch_dictionary_advertised_);
  job_->sdch_dictionary_advertised_ = false;
}

bool URLRequestHttpJob::HttpFilterContext::IsSdchResponse() const {
  return job_->sdch_dictionary_advertised_;
}

int64 URLRequestHttpJob::HttpFilterContext::GetByteReadCount() const {
  return job_->filter_input_byte_count();
}

int URLRequestHttpJob::HttpFilterContext::GetResponseCode() const {
  return job_->GetResponseCode();
}

void URLRequestHttpJob::HttpFilterContext::RecordPacketStats(
    StatisticSelector statistic) const {
  job_->RecordPacketStats(statistic);
}

// TODO(darin): make sure the port blocking code is not lost
// static
URLRequestJob* URLRequestHttpJob::Factory(URLRequest* request,
                                          NetworkDelegate* network_delegate,
                                          const std::string& scheme) {
  DCHECK(scheme == "http" || scheme == "https");

  if (!request->context()->http_transaction_factory()) {
    NOTREACHED() << "requires a valid context";
    return new URLRequestErrorJob(
        request, network_delegate, ERR_INVALID_ARGUMENT);
  }

  GURL redirect_url;
  if (request->GetHSTSRedirect(&redirect_url)) {
    return new URLRequestRedirectJob(
        request, network_delegate, redirect_url,
        // Use status code 307 to preserve the method, so POST requests work.
        URLRequestRedirectJob::REDIRECT_307_TEMPORARY_REDIRECT);
  }
  return new URLRequestHttpJob(request,
                               network_delegate,
                               request->context()->http_user_agent_settings());
}


URLRequestHttpJob::URLRequestHttpJob(
    URLRequest* request,
    NetworkDelegate* network_delegate,
    const HttpUserAgentSettings* http_user_agent_settings)
    : URLRequestJob(request, network_delegate),
      response_info_(NULL),
      response_cookies_save_index_(0),
      proxy_auth_state_(AUTH_STATE_DONT_NEED_AUTH),
      server_auth_state_(AUTH_STATE_DONT_NEED_AUTH),
      ALLOW_THIS_IN_INITIALIZER_LIST(start_callback_(
          base::Bind(&URLRequestHttpJob::OnStartCompleted,
                     base::Unretained(this)))),
      ALLOW_THIS_IN_INITIALIZER_LIST(notify_before_headers_sent_callback_(
          base::Bind(&URLRequestHttpJob::NotifyBeforeSendHeadersCallback,
                     base::Unretained(this)))),
      read_in_progress_(false),
      transaction_(NULL),
      throttling_entry_(NULL),
      sdch_dictionary_advertised_(false),
      sdch_test_activated_(false),
      sdch_test_control_(false),
      is_cached_content_(false),
      request_creation_time_(),
      packet_timing_enabled_(false),
      done_(false),
      bytes_observed_in_packets_(0),
      request_time_snapshot_(),
      final_packet_time_(),
      ALLOW_THIS_IN_INITIALIZER_LIST(
          filter_context_(new HttpFilterContext(this))),
      ALLOW_THIS_IN_INITIALIZER_LIST(weak_factory_(this)),
      ALLOW_THIS_IN_INITIALIZER_LIST(on_headers_received_callback_(
          base::Bind(&URLRequestHttpJob::OnHeadersReceivedCallback,
                     base::Unretained(this)))),
      awaiting_callback_(false),
      http_transaction_delegate_(new HttpTransactionDelegateImpl(request)),
      http_user_agent_settings_(http_user_agent_settings) {
  URLRequestThrottlerManager* manager = request->context()->throttler_manager();
  if (manager)
    throttling_entry_ = manager->RegisterRequestUrl(request->url());

  ResetTimer();
}

void URLRequestHttpJob::NotifyHeadersComplete() {
  DCHECK(!response_info_);

  response_info_ = transaction_->GetResponseInfo();

  // Save boolean, as we'll need this info at destruction time, and filters may
  // also need this info.
  is_cached_content_ = response_info_->was_cached;

  if (!is_cached_content_ && throttling_entry_) {
    URLRequestThrottlerHeaderAdapter response_adapter(GetResponseHeaders());
    throttling_entry_->UpdateWithResponse(request_info_.url.host(),
                                          &response_adapter);
  }

  // The ordering of these calls is not important.
  ProcessStrictTransportSecurityHeader();
  ProcessPublicKeyPinsHeader();

#if !defined(__LB_SHELL__)
  if (SdchManager::Global() &&
      SdchManager::Global()->IsInSupportedDomain(request_->url())) {
    const std::string name = "Get-Dictionary";
    std::string url_text;
    void* iter = NULL;
    // TODO(jar): We need to not fetch dictionaries the first time they are
    // seen, but rather wait until we can justify their usefulness.
    // For now, we will only fetch the first dictionary, which will at least
    // require multiple suggestions before we get additional ones for this site.
    // Eventually we should wait until a dictionary is requested several times
    // before we even download it (so that we don't waste memory or bandwidth).
    if (GetResponseHeaders()->EnumerateHeader(&iter, name, &url_text)) {
      // request_->url() won't be valid in the destructor, so we use an
      // alternate copy.
      DCHECK_EQ(request_->url(), request_info_.url);
      // Resolve suggested URL relative to request url.
      sdch_dictionary_url_ = request_info_.url.Resolve(url_text);
    }
  }
#endif

  // The HTTP transaction may be restarted several times for the purposes
  // of sending authorization information. Each time it restarts, we get
  // notified of the headers completion so that we can update the cookie store.
  if (transaction_->IsReadyToRestartForAuth()) {
    DCHECK(!response_info_->auth_challenge.get());
    // TODO(battre): This breaks the webrequest API for
    // URLRequestTestHTTP.BasicAuthWithCookies
    // where OnBeforeSendHeaders -> OnSendHeaders -> OnBeforeSendHeaders
    // occurs.
    RestartTransactionWithAuth(AuthCredentials());
    return;
  }

  URLRequestJob::NotifyHeadersComplete();
}

void URLRequestHttpJob::NotifyDone(const URLRequestStatus& status) {
  DoneWithRequest(FINISHED);
  URLRequestJob::NotifyDone(status);
}

void URLRequestHttpJob::DestroyTransaction() {
  DCHECK(transaction_.get());

  DoneWithRequest(ABORTED);
  transaction_.reset();
  response_info_ = NULL;
}

void URLRequestHttpJob::StartTransaction() {
  if (request_->context()->network_delegate()) {
    int rv = request_->context()->network_delegate()->NotifyBeforeSendHeaders(
        request_, notify_before_headers_sent_callback_,
        &request_info_.extra_headers);
    // If an extension blocks the request, we rely on the callback to
    // MaybeStartTransactionInternal().
    if (rv == ERR_IO_PENDING) {
      SetBlockedOnDelegate();
      return;
    }
    MaybeStartTransactionInternal(rv);
    return;
  }
  StartTransactionInternal();
}

void URLRequestHttpJob::NotifyBeforeSendHeadersCallback(int result) {
  SetUnblockedOnDelegate();

  // Check that there are no callbacks to already canceled requests.
  DCHECK_NE(URLRequestStatus::CANCELED, GetStatus().status());

  MaybeStartTransactionInternal(result);
}

void URLRequestHttpJob::MaybeStartTransactionInternal(int result) {
  if (result == OK) {
    StartTransactionInternal();
  } else {
    std::string source("delegate");
    request_->net_log().AddEvent(NetLog::TYPE_CANCELLED,
                                 NetLog::StringCallback("source", &source));
    NotifyCanceled();
    NotifyStartError(URLRequestStatus(URLRequestStatus::FAILED, result));
  }
}

void URLRequestHttpJob::StartTransactionInternal() {
  // NOTE: This method assumes that request_info_ is already setup properly.

  // If we already have a transaction, then we should restart the transaction
  // with auth provided by auth_credentials_.

  int rv;

  if (request_->context()->network_delegate()) {
    request_->context()->network_delegate()->NotifySendHeaders(
        request_, request_info_.extra_headers);
  }

  if (transaction_.get()) {
    rv = transaction_->RestartWithAuth(auth_credentials_, start_callback_);
    auth_credentials_ = AuthCredentials();
  } else {
    DCHECK(request_->context()->http_transaction_factory());

    rv = request_->context()->http_transaction_factory()->CreateTransaction(
        &transaction_, http_transaction_delegate_.get());
    if (rv == OK) {
      if (!throttling_entry_ ||
          !throttling_entry_->ShouldRejectRequest(*request_)) {
        rv = transaction_->Start(
            &request_info_, start_callback_, request_->net_log());
        start_time_ = base::TimeTicks::Now();
      } else {
        // Special error code for the exponential back-off module.
        rv = ERR_TEMPORARILY_THROTTLED;
      }
    }
  }

  if (rv == ERR_IO_PENDING)
    return;

  // The transaction started synchronously, but we need to notify the
  // URLRequest delegate via the message loop.
  MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&URLRequestHttpJob::OnStartCompleted,
                 weak_factory_.GetWeakPtr(), rv));
}

void URLRequestHttpJob::AddExtraHeaders() {
  // Supply Accept-Encoding field only if it is not already provided.
  // It should be provided IF the content is known to have restrictions on
  // potential encoding, such as streaming multi-media.
  // For details see bug 47381.
  // TODO(jar, enal): jpeg files etc. should set up a request header if
  // possible. Right now it is done only by buffered_resource_loader and
  // simple_data_source.
  if (!request_info_.extra_headers.HasHeader(
      HttpRequestHeaders::kAcceptEncoding)) {
#if !defined(__LB_SHELL__)
    bool advertise_sdch = SdchManager::Global() &&
        SdchManager::Global()->IsInSupportedDomain(request_->url());
    std::string avail_dictionaries;
    if (advertise_sdch) {
      SdchManager::Global()->GetAvailDictionaryList(request_->url(),
                                                    &avail_dictionaries);

      // The AllowLatencyExperiment() is only true if we've successfully done a
      // full SDCH compression recently in this browser session for this host.
      // Note that for this path, there might be no applicable dictionaries,
      // and hence we can't participate in the experiment.
      if (!avail_dictionaries.empty() &&
          SdchManager::Global()->AllowLatencyExperiment(request_->url())) {
        // We are participating in the test (or control), and hence we'll
        // eventually record statistics via either SDCH_EXPERIMENT_DECODE or
        // SDCH_EXPERIMENT_HOLDBACK, and we'll need some packet timing data.
        packet_timing_enabled_ = true;
        if (base::RandDouble() < .01) {
          sdch_test_control_ = true;  // 1% probability.
          advertise_sdch = false;
        } else {
          sdch_test_activated_ = true;
        }
      }
    }
#else
    bool advertise_sdch = false;
#endif

    // Supply Accept-Encoding headers first so that it is more likely that they
    // will be in the first transmitted packet.  This can sometimes make it
    // easier to filter and analyze the streams to assure that a proxy has not
    // damaged these headers.  Some proxies deliberately corrupt Accept-Encoding
    // headers.
    if (!advertise_sdch) {
      // Tell the server what compression formats we support (other than SDCH).
      request_info_.extra_headers.SetHeader(
          HttpRequestHeaders::kAcceptEncoding, "gzip,deflate");
    } else {
#if !defined(__LB_SHELL__)
      // Include SDCH in acceptable list.
      request_info_.extra_headers.SetHeader(
          HttpRequestHeaders::kAcceptEncoding, "gzip,deflate,sdch");
      if (!avail_dictionaries.empty()) {
        request_info_.extra_headers.SetHeader(
            kAvailDictionaryHeader,
            avail_dictionaries);
        sdch_dictionary_advertised_ = true;
        // Since we're tagging this transaction as advertising a dictionary,
        // we'll definitely employ an SDCH filter (or tentative sdch filter)
        // when we get a response.  When done, we'll record histograms via
        // SDCH_DECODE or SDCH_PASSTHROUGH.  Hence we need to record packet
        // arrival times.
        packet_timing_enabled_ = true;
      }
#endif
    }
  }

  if (http_user_agent_settings_) {
    // Only add default Accept-Language and Accept-Charset if the request
    // didn't have them specified.
    std::string accept_language =
        http_user_agent_settings_->GetAcceptLanguage();
    if (!accept_language.empty()) {
      request_info_.extra_headers.SetHeaderIfMissing(
          HttpRequestHeaders::kAcceptLanguage,
          accept_language);
    }
    std::string accept_charset = http_user_agent_settings_->GetAcceptCharset();
    if (!accept_charset.empty()) {
      request_info_.extra_headers.SetHeaderIfMissing(
          HttpRequestHeaders::kAcceptCharset,
          accept_charset);
    }
  }
}

void URLRequestHttpJob::AddCookieHeaderAndStart() {
  // No matter what, we want to report our status as IO pending since we will
  // be notifying our consumer asynchronously via OnStartCompleted.
  SetStatus(URLRequestStatus(URLRequestStatus::IO_PENDING, 0));

  // If the request was destroyed, then there is no more work to do.
  if (!request_)
    return;

  CookieStore* cookie_store = request_->context()->cookie_store();
  if (cookie_store && !(request_info_.load_flags & LOAD_DO_NOT_SEND_COOKIES)) {
    net::CookieMonster* cookie_monster = cookie_store->GetCookieMonster();
    if (cookie_monster) {
      cookie_monster->GetAllCookiesForURLAsync(
          request_->url(),
          base::Bind(&URLRequestHttpJob::CheckCookiePolicyAndLoad,
                     weak_factory_.GetWeakPtr()));
    } else {
      CheckCookiePolicyAndLoad(CookieList());
    }
  } else {
    DoStartTransaction();
  }
}

void URLRequestHttpJob::DoLoadCookies() {
  CookieOptions options;
  options.set_include_httponly();
  request_->context()->cookie_store()->GetCookiesWithInfoAsync(
      request_->url(), options,
      base::Bind(&URLRequestHttpJob::OnCookiesLoaded,
                 weak_factory_.GetWeakPtr()));
}

void URLRequestHttpJob::CheckCookiePolicyAndLoad(
    const CookieList& cookie_list) {
  if (CanGetCookies(cookie_list))
    DoLoadCookies();
  else
    DoStartTransaction();
}

void URLRequestHttpJob::OnCookiesLoaded(
    const std::string& cookie_line,
    const std::vector<net::CookieStore::CookieInfo>& cookie_infos) {
  if (!cookie_line.empty()) {
    request_info_.extra_headers.SetHeader(
        HttpRequestHeaders::kCookie, cookie_line);
  }
  DoStartTransaction();
}

void URLRequestHttpJob::DoStartTransaction() {
  // We may have been canceled while retrieving cookies.
  if (GetStatus().is_success()) {
    StartTransaction();
  } else {
    NotifyCanceled();
  }
}

void URLRequestHttpJob::SaveCookiesAndNotifyHeadersComplete(int result) {
  if (result != net::OK) {
    std::string source("delegate");
    request_->net_log().AddEvent(NetLog::TYPE_CANCELLED,
                                 NetLog::StringCallback("source", &source));
    NotifyStartError(URLRequestStatus(URLRequestStatus::FAILED, result));
    return;
  }

  DCHECK(transaction_.get());

  const HttpResponseInfo* response_info = transaction_->GetResponseInfo();
  DCHECK(response_info);

  response_cookies_.clear();
  response_cookies_save_index_ = 0;

  FetchResponseCookies(&response_cookies_);

  if (!GetResponseHeaders()->GetDateValue(&response_date_))
    response_date_ = base::Time();

  // Now, loop over the response cookies, and attempt to persist each.
  SaveNextCookie();
}

// If the save occurs synchronously, SaveNextCookie will loop and save the next
// cookie. If the save is deferred, the callback is responsible for continuing
// to iterate through the cookies.
// TODO(erikwright): Modify the CookieStore API to indicate via return value
// whether it completed synchronously or asynchronously.
// See http://crbug.com/131066.
void URLRequestHttpJob::SaveNextCookie() {
  // No matter what, we want to report our status as IO pending since we will
  // be notifying our consumer asynchronously via OnStartCompleted.
  SetStatus(URLRequestStatus(URLRequestStatus::IO_PENDING, 0));

  // Used to communicate with the callback. See the implementation of
  // OnCookieSaved.
  scoped_refptr<SharedBoolean> callback_pending = new SharedBoolean(false);
  scoped_refptr<SharedBoolean> save_next_cookie_running =
      new SharedBoolean(true);

  if (!(request_info_.load_flags & LOAD_DO_NOT_SAVE_COOKIES) &&
      request_->context()->cookie_store() &&
      response_cookies_.size() > 0) {
    CookieOptions options;
    options.set_include_httponly();
    options.set_server_time(response_date_);

    net::CookieStore::SetCookiesCallback callback(
        base::Bind(&URLRequestHttpJob::OnCookieSaved,
                   weak_factory_.GetWeakPtr(),
                   save_next_cookie_running,
                   callback_pending));

    // Loop through the cookies as long as SetCookieWithOptionsAsync completes
    // synchronously.
    while (!callback_pending->data &&
           response_cookies_save_index_ < response_cookies_.size()) {
      if (CanSetCookie(
          response_cookies_[response_cookies_save_index_], &options)) {
        callback_pending->data = true;
        request_->context()->cookie_store()->SetCookieWithOptionsAsync(
            request_->url(), response_cookies_[response_cookies_save_index_],
            options, callback);
      }
      ++response_cookies_save_index_;
    }
  }

  save_next_cookie_running->data = false;

  if (!callback_pending->data) {
    response_cookies_.clear();
    response_cookies_save_index_ = 0;
    SetStatus(URLRequestStatus());  // Clear the IO_PENDING status
    NotifyHeadersComplete();
    return;
  }
}

// |save_next_cookie_running| is true when the callback is bound and set to
// false when SaveNextCookie exits, allowing the callback to determine if the
// save occurred synchronously or asynchronously.
// |callback_pending| is false when the callback is invoked and will be set to
// true by the callback, allowing SaveNextCookie to detect whether the save
// occurred synchronously.
// See SaveNextCookie() for more information.
void URLRequestHttpJob::OnCookieSaved(
    scoped_refptr<SharedBoolean> save_next_cookie_running,
    scoped_refptr<SharedBoolean> callback_pending,
    bool cookie_status) {
  callback_pending->data = false;

  // If we were called synchronously, return.
  if (save_next_cookie_running->data) {
    return;
  }

  // We were called asynchronously, so trigger the next save.
  // We may have been canceled within OnSetCookie.
  if (GetStatus().is_success()) {
    SaveNextCookie();
  } else {
    NotifyCanceled();
  }
}

void URLRequestHttpJob::FetchResponseCookies(
    std::vector<std::string>* cookies) {
  const std::string name = "Set-Cookie";
  std::string value;

  void* iter = NULL;
  HttpResponseHeaders* headers = GetResponseHeaders();
  while (headers->EnumerateHeader(&iter, name, &value)) {
    if (!value.empty())
      cookies->push_back(value);
  }
}

// NOTE: |ProcessStrictTransportSecurityHeader| and
// |ProcessPublicKeyPinsHeader| have very similar structures, by design.
// They manipulate different parts of |TransportSecurityState::DomainState|,
// and they must remain complementary. If, in future changes here, there is
// any conflict between their policies (such as in |domain_state.mode|), you
// should resolve the conflict in favor of the more strict policy.
void URLRequestHttpJob::ProcessStrictTransportSecurityHeader() {
  DCHECK(response_info_);

  const URLRequestContext* ctx = request_->context();
  const SSLInfo& ssl_info = response_info_->ssl_info;

  // Only accept strict transport security headers on HTTPS connections that
  // have no certificate errors.
  if (!ssl_info.is_valid() || IsCertStatusError(ssl_info.cert_status) ||
      !ctx->transport_security_state()) {
    return;
  }

  TransportSecurityState* security_state = ctx->transport_security_state();
  TransportSecurityState::DomainState domain_state;
  const std::string& host = request_info_.url.host();

  bool sni_available =
      SSLConfigService::IsSNIAvailable(ctx->ssl_config_service());
  if (!security_state->GetDomainState(host, sni_available, &domain_state))
    // |GetDomainState| may have altered |domain_state| while searching. If
    // not found, start with a fresh state.
    domain_state.upgrade_mode =
        TransportSecurityState::DomainState::MODE_FORCE_HTTPS;

  HttpResponseHeaders* headers = GetResponseHeaders();
  std::string value;
  void* iter = NULL;
  base::Time now = base::Time::Now();

  // http://tools.ietf.org/html/draft-ietf-websec-strict-transport-sec:
  //
  //   If a UA receives more than one STS header field in a HTTP response
  //   message over secure transport, then the UA MUST process only the
  //   first such header field.
  bool seen_sts = false;
  while (headers->EnumerateHeader(&iter, "Strict-Transport-Security", &value)) {
    if (seen_sts)
      return;
    seen_sts = true;
    TransportSecurityState::DomainState domain_state;
    if (domain_state.ParseSTSHeader(now, value))
      security_state->EnableHost(host, domain_state);
  }
}

void URLRequestHttpJob::ProcessPublicKeyPinsHeader() {
  DCHECK(response_info_);

  const URLRequestContext* ctx = request_->context();
  const SSLInfo& ssl_info = response_info_->ssl_info;

  // Only accept public key pins headers on HTTPS connections that have no
  // certificate errors.
  if (!ssl_info.is_valid() || IsCertStatusError(ssl_info.cert_status) ||
      !ctx->transport_security_state()) {
    return;
  }

  TransportSecurityState* security_state = ctx->transport_security_state();
  TransportSecurityState::DomainState domain_state;
  const std::string& host = request_info_.url.host();

  bool sni_available =
      SSLConfigService::IsSNIAvailable(ctx->ssl_config_service());
  if (!security_state->GetDomainState(host, sni_available, &domain_state))
    // |GetDomainState| may have altered |domain_state| while searching. If
    // not found, start with a fresh state.
    domain_state.upgrade_mode =
        TransportSecurityState::DomainState::MODE_DEFAULT;

  HttpResponseHeaders* headers = GetResponseHeaders();
  void* iter = NULL;
  std::string value;
  base::Time now = base::Time::Now();

  while (headers->EnumerateHeader(&iter, "Public-Key-Pins", &value)) {
    // Note that ParsePinsHeader updates |domain_state| (iff the header parses
    // correctly), but does not completely overwrite it. It just updates the
    // dynamic pinning metadata.
    if (domain_state.ParsePinsHeader(now, value, ssl_info))
      security_state->EnableHost(host, domain_state);
  }
}

void URLRequestHttpJob::OnStartCompleted(int result) {
  RecordTimer();

  // If the request was destroyed, then there is no more work to do.
  if (!request_)
    return;

  // If the transaction was destroyed, then the job was cancelled, and
  // we can just ignore this notification.
  if (!transaction_.get())
    return;

  // Clear the IO_PENDING status
  SetStatus(URLRequestStatus());

  const URLRequestContext* context = request_->context();

  if (result == ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN &&
      transaction_->GetResponseInfo() != NULL) {
    FraudulentCertificateReporter* reporter =
      context->fraudulent_certificate_reporter();
    if (reporter != NULL) {
      const SSLInfo& ssl_info = transaction_->GetResponseInfo()->ssl_info;
      bool sni_available = SSLConfigService::IsSNIAvailable(
          context->ssl_config_service());
      const std::string& host = request_->url().host();

      reporter->SendReport(host, ssl_info, sni_available);
    }
  }

  if (result == OK) {
    scoped_refptr<HttpResponseHeaders> headers = GetResponseHeaders();
    if (context->network_delegate()) {
      // Note that |this| may not be deleted until
      // |on_headers_received_callback_| or
      // |NetworkDelegate::URLRequestDestroyed()| has been called.
      int error = context->network_delegate()->
          NotifyHeadersReceived(request_, on_headers_received_callback_,
                                headers, &override_response_headers_);
      if (error != net::OK) {
        if (error == net::ERR_IO_PENDING) {
          awaiting_callback_ = true;
          SetBlockedOnDelegate();
        } else {
          std::string source("delegate");
          request_->net_log().AddEvent(NetLog::TYPE_CANCELLED,
                                       NetLog::StringCallback("source",
                                                              &source));
          NotifyStartError(URLRequestStatus(URLRequestStatus::FAILED, error));
        }
        return;
      }
    }

    SaveCookiesAndNotifyHeadersComplete(net::OK);
  } else if (IsCertificateError(result)) {
    // We encountered an SSL certificate error.  Ask our delegate to decide
    // what we should do.

    TransportSecurityState::DomainState domain_state;
    const URLRequestContext* context = request_->context();
    const bool fatal =
        context->transport_security_state() &&
        context->transport_security_state()->GetDomainState(
            request_info_.url.host(),
            SSLConfigService::IsSNIAvailable(context->ssl_config_service()),
            &domain_state);
    NotifySSLCertificateError(transaction_->GetResponseInfo()->ssl_info, fatal);
  } else if (result == ERR_SSL_CLIENT_AUTH_CERT_NEEDED) {
    NotifyCertificateRequested(
        transaction_->GetResponseInfo()->cert_request_info);
  } else {
    NotifyStartError(URLRequestStatus(URLRequestStatus::FAILED, result));
  }
}

void URLRequestHttpJob::OnHeadersReceivedCallback(int result) {
  SetUnblockedOnDelegate();
  awaiting_callback_ = false;

  // Check that there are no callbacks to already canceled requests.
  DCHECK_NE(URLRequestStatus::CANCELED, GetStatus().status());

  SaveCookiesAndNotifyHeadersComplete(result);
}

void URLRequestHttpJob::OnReadCompleted(int result) {
  read_in_progress_ = false;

  if (ShouldFixMismatchedContentLength(result))
    result = OK;

  if (result == OK) {
    NotifyDone(URLRequestStatus());
  } else if (result < 0) {
    NotifyDone(URLRequestStatus(URLRequestStatus::FAILED, result));
  } else {
    // Clear the IO_PENDING status
    SetStatus(URLRequestStatus());
  }

  NotifyReadComplete(result);
}

void URLRequestHttpJob::RestartTransactionWithAuth(
    const AuthCredentials& credentials) {
  auth_credentials_ = credentials;

  // These will be reset in OnStartCompleted.
  response_info_ = NULL;
  response_cookies_.clear();

  ResetTimer();

  // Update the cookies, since the cookie store may have been updated from the
  // headers in the 401/407. Since cookies were already appended to
  // extra_headers, we need to strip them out before adding them again.
  request_info_.extra_headers.RemoveHeader(HttpRequestHeaders::kCookie);

  AddCookieHeaderAndStart();
}

void URLRequestHttpJob::SetUpload(UploadDataStream* upload) {
  DCHECK(!transaction_.get()) << "cannot change once started";
  request_info_.upload_data_stream = upload;
}

void URLRequestHttpJob::SetExtraRequestHeaders(
    const HttpRequestHeaders& headers) {
  DCHECK(!transaction_.get()) << "cannot change once started";
  request_info_.extra_headers.CopyFrom(headers);
}

void URLRequestHttpJob::Start() {
  DCHECK(!transaction_.get());

  // Ensure that we do not send username and password fields in the referrer.
  GURL referrer(request_->GetSanitizedReferrer());

  request_info_.url = request_->url();
  request_info_.method = request_->method();
  request_info_.load_flags = request_->load_flags();
  request_info_.priority = request_->priority();
  request_info_.request_id = request_->identifier();

  // Strip Referer from request_info_.extra_headers to prevent, e.g., plugins
  // from overriding headers that are controlled using other means. Otherwise a
  // plugin could set a referrer although sending the referrer is inhibited.
  request_info_.extra_headers.RemoveHeader(HttpRequestHeaders::kReferer);

  // Our consumer should have made sure that this is a safe referrer.  See for
  // instance WebCore::FrameLoader::HideReferrer.
  if (referrer.is_valid()) {
    request_info_.extra_headers.SetHeader(HttpRequestHeaders::kReferer,
                                          referrer.spec());
  }

  request_info_.extra_headers.SetHeaderIfMissing(
      HttpRequestHeaders::kUserAgent,
      http_user_agent_settings_ ?
          http_user_agent_settings_->GetUserAgent(request_->url()) :
          EmptyString());

  AddExtraHeaders();
  AddCookieHeaderAndStart();
}

void URLRequestHttpJob::Kill() {
  http_transaction_delegate_->OnDetachRequest();

  if (!transaction_.get())
    return;

  weak_factory_.InvalidateWeakPtrs();
  DestroyTransaction();
  URLRequestJob::Kill();
}

LoadState URLRequestHttpJob::GetLoadState() const {
  return transaction_.get() ?
      transaction_->GetLoadState() : LOAD_STATE_IDLE;
}

UploadProgress URLRequestHttpJob::GetUploadProgress() const {
  return transaction_.get() ?
      transaction_->GetUploadProgress() : UploadProgress();
}

bool URLRequestHttpJob::GetMimeType(std::string* mime_type) const {
  DCHECK(transaction_.get());

  if (!response_info_)
    return false;

  return GetResponseHeaders()->GetMimeType(mime_type);
}

bool URLRequestHttpJob::GetCharset(std::string* charset) {
  DCHECK(transaction_.get());

  if (!response_info_)
    return false;

  return GetResponseHeaders()->GetCharset(charset);
}

void URLRequestHttpJob::GetResponseInfo(HttpResponseInfo* info) {
  DCHECK(request_);
  DCHECK(transaction_.get());

  if (response_info_) {
    *info = *response_info_;
    if (override_response_headers_)
      info->headers = override_response_headers_;
  }
}

bool URLRequestHttpJob::GetResponseCookies(std::vector<std::string>* cookies) {
  DCHECK(transaction_.get());

  if (!response_info_)
    return false;

  // TODO(darin): Why are we extracting response cookies again?  Perhaps we
  // should just leverage response_cookies_.

  cookies->clear();
  FetchResponseCookies(cookies);
  return true;
}

int URLRequestHttpJob::GetResponseCode() const {
  DCHECK(transaction_.get());

  if (!response_info_)
    return -1;

  return GetResponseHeaders()->response_code();
}

Filter* URLRequestHttpJob::SetupFilter() const {
  DCHECK(transaction_.get());
  if (!response_info_)
    return NULL;

  std::vector<Filter::FilterType> encoding_types;
  std::string encoding_type;
  HttpResponseHeaders* headers = GetResponseHeaders();
  void* iter = NULL;
  while (headers->EnumerateHeader(&iter, "Content-Encoding", &encoding_type)) {
    encoding_types.push_back(Filter::ConvertEncodingToType(encoding_type));
  }

  if (filter_context_->IsSdchResponse()) {
    // We are wary of proxies that discard or damage SDCH encoding.  If a server
    // explicitly states that this is not SDCH content, then we can correct our
    // assumption that this is an SDCH response, and avoid the need to recover
    // as though the content is corrupted (when we discover it is not SDCH
    // encoded).
    std::string sdch_response_status;
    iter = NULL;
    while (headers->EnumerateHeader(&iter, "X-Sdch-Encode",
                                    &sdch_response_status)) {
      if (sdch_response_status == "0") {
        filter_context_->ResetSdchResponseToFalse();
        break;
      }
    }
  }

  // Even if encoding types are empty, there is a chance that we need to add
  // some decoding, as some proxies strip encoding completely. In such cases,
  // we may need to add (for example) SDCH filtering (when the context suggests
  // it is appropriate).
  Filter::FixupEncodingTypes(*filter_context_, &encoding_types);

  return !encoding_types.empty()
      ? Filter::Factory(encoding_types, *filter_context_) : NULL;
}

bool URLRequestHttpJob::IsSafeRedirect(const GURL& location) {
  // We only allow redirects to certain "safe" protocols.  This does not
  // restrict redirects to externally handled protocols.  Our consumer would
  // need to take care of those.

  if (!URLRequest::IsHandledURL(location))
    return true;

  static const char* kSafeSchemes[] = {
    "http",
    "https",
    "ftp"
  };

  for (size_t i = 0; i < arraysize(kSafeSchemes); ++i) {
    if (location.SchemeIs(kSafeSchemes[i]))
      return true;
  }

  return false;
}

bool URLRequestHttpJob::NeedsAuth() {
  int code = GetResponseCode();
  if (code == -1)
    return false;

  // Check if we need either Proxy or WWW Authentication.  This could happen
  // because we either provided no auth info, or provided incorrect info.
  switch (code) {
    case 407:
      if (proxy_auth_state_ == AUTH_STATE_CANCELED)
        return false;
      proxy_auth_state_ = AUTH_STATE_NEED_AUTH;
      return true;
    case 401:
      if (server_auth_state_ == AUTH_STATE_CANCELED)
        return false;
      server_auth_state_ = AUTH_STATE_NEED_AUTH;
      return true;
  }
  return false;
}

void URLRequestHttpJob::GetAuthChallengeInfo(
    scoped_refptr<AuthChallengeInfo>* result) {
  DCHECK(transaction_.get());
  DCHECK(response_info_);

  // sanity checks:
  DCHECK(proxy_auth_state_ == AUTH_STATE_NEED_AUTH ||
         server_auth_state_ == AUTH_STATE_NEED_AUTH);
  DCHECK((GetResponseHeaders()->response_code() == HTTP_UNAUTHORIZED) ||
         (GetResponseHeaders()->response_code() ==
          HTTP_PROXY_AUTHENTICATION_REQUIRED));

  *result = response_info_->auth_challenge;
}

void URLRequestHttpJob::SetAuth(const AuthCredentials& credentials) {
  DCHECK(transaction_.get());

  // Proxy gets set first, then WWW.
  if (proxy_auth_state_ == AUTH_STATE_NEED_AUTH) {
    proxy_auth_state_ = AUTH_STATE_HAVE_AUTH;
  } else {
    DCHECK_EQ(server_auth_state_, AUTH_STATE_NEED_AUTH);
    server_auth_state_ = AUTH_STATE_HAVE_AUTH;
  }

  RestartTransactionWithAuth(credentials);
}

void URLRequestHttpJob::CancelAuth() {
  // Proxy gets set first, then WWW.
  if (proxy_auth_state_ == AUTH_STATE_NEED_AUTH) {
    proxy_auth_state_ = AUTH_STATE_CANCELED;
  } else {
    DCHECK_EQ(server_auth_state_, AUTH_STATE_NEED_AUTH);
    server_auth_state_ = AUTH_STATE_CANCELED;
  }

  // These will be reset in OnStartCompleted.
  response_info_ = NULL;
  response_cookies_.clear();

  ResetTimer();

  // OK, let the consumer read the error page...
  //
  // Because we set the AUTH_STATE_CANCELED flag, NeedsAuth will return false,
  // which will cause the consumer to receive OnResponseStarted instead of
  // OnAuthRequired.
  //
  // We have to do this via InvokeLater to avoid "recursing" the consumer.
  //
  MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&URLRequestHttpJob::OnStartCompleted,
                 weak_factory_.GetWeakPtr(), OK));
}

void URLRequestHttpJob::ContinueWithCertificate(
    X509Certificate* client_cert) {
  DCHECK(transaction_.get());

  DCHECK(!response_info_) << "should not have a response yet";

  ResetTimer();

  // No matter what, we want to report our status as IO pending since we will
  // be notifying our consumer asynchronously via OnStartCompleted.
  SetStatus(URLRequestStatus(URLRequestStatus::IO_PENDING, 0));

  int rv = transaction_->RestartWithCertificate(client_cert, start_callback_);
  if (rv == ERR_IO_PENDING)
    return;

  // The transaction started synchronously, but we need to notify the
  // URLRequest delegate via the message loop.
  MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&URLRequestHttpJob::OnStartCompleted,
                 weak_factory_.GetWeakPtr(), rv));
}

void URLRequestHttpJob::ContinueDespiteLastError() {
  // If the transaction was destroyed, then the job was cancelled.
  if (!transaction_.get())
    return;

  DCHECK(!response_info_) << "should not have a response yet";

  ResetTimer();

  // No matter what, we want to report our status as IO pending since we will
  // be notifying our consumer asynchronously via OnStartCompleted.
  SetStatus(URLRequestStatus(URLRequestStatus::IO_PENDING, 0));

  int rv = transaction_->RestartIgnoringLastError(start_callback_);
  if (rv == ERR_IO_PENDING)
    return;

  // The transaction started synchronously, but we need to notify the
  // URLRequest delegate via the message loop.
  MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&URLRequestHttpJob::OnStartCompleted,
                 weak_factory_.GetWeakPtr(), rv));
}

bool URLRequestHttpJob::ShouldFixMismatchedContentLength(int rv) const {
  // Some servers send the body compressed, but specify the content length as
  // the uncompressed size.  Although this violates the HTTP spec we want to
  // support it (as IE and FireFox do), but *only* for an exact match.
  // See http://crbug.com/79694.
  if (rv == net::ERR_CONTENT_LENGTH_MISMATCH ||
      rv == net::ERR_INCOMPLETE_CHUNKED_ENCODING) {
    if (request_ && request_->response_headers()) {
      int64 expected_length = request_->response_headers()->GetContentLength();
      VLOG(1) << __FUNCTION__ << "() "
              << "\"" << request_->url().spec() << "\""
              << " content-length = " << expected_length
              << " pre total = " << prefilter_bytes_read()
              << " post total = " << postfilter_bytes_read();
      if (postfilter_bytes_read() == expected_length) {
        // Clear the error.
        return true;
      }
    }
  }
  return false;
}

bool URLRequestHttpJob::ReadRawData(IOBuffer* buf, int buf_size,
                                    int* bytes_read) {
  DCHECK_NE(buf_size, 0);
  DCHECK(bytes_read);
  DCHECK(!read_in_progress_);

  int rv = transaction_->Read(
      buf, buf_size,
      base::Bind(&URLRequestHttpJob::OnReadCompleted, base::Unretained(this)));

  if (ShouldFixMismatchedContentLength(rv))
    rv = 0;

  if (rv >= 0) {
    *bytes_read = rv;
    if (!rv)
      DoneWithRequest(FINISHED);
    return true;
  }

  if (rv == ERR_IO_PENDING) {
    read_in_progress_ = true;
    SetStatus(URLRequestStatus(URLRequestStatus::IO_PENDING, 0));
  } else {
    NotifyDone(URLRequestStatus(URLRequestStatus::FAILED, rv));
  }

  return false;
}

void URLRequestHttpJob::StopCaching() {
  if (transaction_.get())
    transaction_->StopCaching();
}

void URLRequestHttpJob::DoneReading() {
  if (transaction_.get())
    transaction_->DoneReading();
  DoneWithRequest(FINISHED);
}

HostPortPair URLRequestHttpJob::GetSocketAddress() const {
  return response_info_ ? response_info_->socket_address : HostPortPair();
}

URLRequestHttpJob::~URLRequestHttpJob() {
  CHECK(!awaiting_callback_);

#if !defined(__LB_SHELL__)
  DCHECK(!sdch_test_control_ || !sdch_test_activated_);
  if (!is_cached_content_) {
    if (sdch_test_control_)
      RecordPacketStats(FilterContext::SDCH_EXPERIMENT_HOLDBACK);
    if (sdch_test_activated_)
      RecordPacketStats(FilterContext::SDCH_EXPERIMENT_DECODE);
  }
  // Make sure SDCH filters are told to emit histogram data while
  // filter_context_ is still alive.
  DestroyFilters();

  if (sdch_dictionary_url_.is_valid()) {
    // Prior to reaching the destructor, request_ has been set to a NULL
    // pointer, so request_->url() is no longer valid in the destructor, and we
    // use an alternate copy |request_info_.url|.
    SdchManager* manager = SdchManager::Global();
    // To be extra safe, since this is a "different time" from when we decided
    // to get the dictionary, we'll validate that an SdchManager is available.
    // At shutdown time, care is taken to be sure that we don't delete this
    // globally useful instance "too soon," so this check is just defensive
    // coding to assure that IF the system is shutting down, we don't have any
    // problem if the manager was deleted ahead of time.
    if (manager)  // Defensive programming.
      manager->FetchDictionary(request_info_.url, sdch_dictionary_url_);
  }
#endif
  DoneWithRequest(ABORTED);
}

void URLRequestHttpJob::RecordTimer() {
  if (request_creation_time_.is_null()) {
    NOTREACHED()
        << "The same transaction shouldn't start twice without new timing.";
    return;
  }

  base::TimeDelta to_start = base::Time::Now() - request_creation_time_;
  request_creation_time_ = base::Time();

  UMA_HISTOGRAM_MEDIUM_TIMES("Net.HttpTimeToFirstByte", to_start);

  static const bool use_overlapped_read_histogram =
      base::FieldTrialList::TrialExists("OverlappedReadImpact");
  if (use_overlapped_read_histogram) {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        base::FieldTrial::MakeName("Net.HttpTimeToFirstByte",
                                   "OverlappedReadImpact"),
        to_start);
  }

  static const bool use_warm_socket_impact_histogram =
      base::FieldTrialList::TrialExists("WarmSocketImpact");
  if (use_warm_socket_impact_histogram) {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        base::FieldTrial::MakeName("Net.HttpTimeToFirstByte",
                                   "WarmSocketImpact"),
        to_start);
  }

  static const bool use_prefetch_histogram =
      base::FieldTrialList::TrialExists("Prefetch");
  if (use_prefetch_histogram) {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        base::FieldTrial::MakeName("Net.HttpTimeToFirstByte",
                                   "Prefetch"),
        to_start);
  }
  static const bool use_prerender_histogram =
      base::FieldTrialList::TrialExists("Prerender");
  if (use_prerender_histogram) {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        base::FieldTrial::MakeName("Net.HttpTimeToFirstByte",
                                   "Prerender"),
        to_start);
  }
}

void URLRequestHttpJob::ResetTimer() {
  if (!request_creation_time_.is_null()) {
    NOTREACHED()
        << "The timer was reset before it was recorded.";
    return;
  }
  request_creation_time_ = base::Time::Now();
}

void URLRequestHttpJob::UpdatePacketReadTimes() {
  if (!packet_timing_enabled_)
    return;

  if (filter_input_byte_count() <= bytes_observed_in_packets_) {
    DCHECK_EQ(filter_input_byte_count(), bytes_observed_in_packets_);
    return;  // No new bytes have arrived.
  }

  final_packet_time_ = base::Time::Now();
  if (!bytes_observed_in_packets_)
    request_time_snapshot_ = request_ ? request_->request_time() : base::Time();

  bytes_observed_in_packets_ = filter_input_byte_count();
}

void URLRequestHttpJob::RecordPacketStats(
    FilterContext::StatisticSelector statistic) const {
  if (!packet_timing_enabled_ || (final_packet_time_ == base::Time()))
    return;

  base::TimeDelta duration = final_packet_time_ - request_time_snapshot_;
  switch (statistic) {
    case FilterContext::SDCH_DECODE: {
      UMA_HISTOGRAM_CUSTOM_COUNTS("Sdch3.Network_Decode_Bytes_Processed_b",
          static_cast<int>(bytes_observed_in_packets_), 500, 100000, 100);
      return;
    }
    case FilterContext::SDCH_PASSTHROUGH: {
      // Despite advertising a dictionary, we handled non-sdch compressed
      // content.
      return;
    }

    case FilterContext::SDCH_EXPERIMENT_DECODE: {
      UMA_HISTOGRAM_CUSTOM_TIMES("Sdch3.Experiment2_Decode",
                                  duration,
                                  base::TimeDelta::FromMilliseconds(20),
                                  base::TimeDelta::FromMinutes(10), 100);
      return;
    }
    case FilterContext::SDCH_EXPERIMENT_HOLDBACK: {
      UMA_HISTOGRAM_CUSTOM_TIMES("Sdch3.Experiment2_Holdback",
                                  duration,
                                  base::TimeDelta::FromMilliseconds(20),
                                  base::TimeDelta::FromMinutes(10), 100);
      return;
    }
    default:
      NOTREACHED();
      return;
  }
}

// The common type of histogram we use for all compression-tracking histograms.
#define COMPRESSION_HISTOGRAM(name, sample) \
    do { \
      UMA_HISTOGRAM_CUSTOM_COUNTS("Net.Compress." name, sample, \
                                  500, 1000000, 100); \
    } while (0)

void URLRequestHttpJob::RecordCompressionHistograms() {
  DCHECK(request_);
  if (!request_)
    return;

  if (is_cached_content_ ||                // Don't record cached content
      !GetStatus().is_success() ||         // Don't record failed content
      !IsCompressibleContent() ||          // Only record compressible content
      !prefilter_bytes_read())       // Zero-byte responses aren't useful.
    return;

  // Miniature requests aren't really compressible.  Don't count them.
  const int kMinSize = 16;
  if (prefilter_bytes_read() < kMinSize)
    return;

  // Only record for http or https urls.
  bool is_http = request_->url().SchemeIs("http");
  bool is_https = request_->url().SchemeIs("https");
  if (!is_http && !is_https)
    return;

  int compressed_B = prefilter_bytes_read();
  int decompressed_B = postfilter_bytes_read();
  bool was_filtered = HasFilter();

  // We want to record how often downloaded resources are compressed.
  // But, we recognize that different protocols may have different
  // properties.  So, for each request, we'll put it into one of 3
  // groups:
  //      a) SSL resources
  //         Proxies cannot tamper with compression headers with SSL.
  //      b) Non-SSL, loaded-via-proxy resources
  //         In this case, we know a proxy might have interfered.
  //      c) Non-SSL, loaded-without-proxy resources
  //         In this case, we know there was no explicit proxy.  However,
  //         it is possible that a transparent proxy was still interfering.
  //
  // For each group, we record the same 3 histograms.

  if (is_https) {
    if (was_filtered) {
      COMPRESSION_HISTOGRAM("SSL.BytesBeforeCompression", compressed_B);
      COMPRESSION_HISTOGRAM("SSL.BytesAfterCompression", decompressed_B);
    } else {
      COMPRESSION_HISTOGRAM("SSL.ShouldHaveBeenCompressed", decompressed_B);
    }
    return;
  }

  if (request_->was_fetched_via_proxy()) {
    if (was_filtered) {
      COMPRESSION_HISTOGRAM("Proxy.BytesBeforeCompression", compressed_B);
      COMPRESSION_HISTOGRAM("Proxy.BytesAfterCompression", decompressed_B);
    } else {
      COMPRESSION_HISTOGRAM("Proxy.ShouldHaveBeenCompressed", decompressed_B);
    }
    return;
  }

  if (was_filtered) {
    COMPRESSION_HISTOGRAM("NoProxy.BytesBeforeCompression", compressed_B);
    COMPRESSION_HISTOGRAM("NoProxy.BytesAfterCompression", decompressed_B);
  } else {
    COMPRESSION_HISTOGRAM("NoProxy.ShouldHaveBeenCompressed", decompressed_B);
  }
}

bool URLRequestHttpJob::IsCompressibleContent() const {
  std::string mime_type;
  return GetMimeType(&mime_type) &&
      (IsSupportedJavascriptMimeType(mime_type.c_str()) ||
       IsSupportedNonImageMimeType(mime_type.c_str()));
}

void URLRequestHttpJob::RecordPerfHistograms(CompletionCause reason) {
  if (start_time_.is_null())
    return;

  base::TimeDelta total_time = base::TimeTicks::Now() - start_time_;
  UMA_HISTOGRAM_TIMES("Net.HttpJob.TotalTime", total_time);

  if (reason == FINISHED) {
    UMA_HISTOGRAM_TIMES("Net.HttpJob.TotalTimeSuccess", total_time);
  } else {
    UMA_HISTOGRAM_TIMES("Net.HttpJob.TotalTimeCancel", total_time);
  }

  if (response_info_) {
    if (response_info_->was_cached) {
      UMA_HISTOGRAM_TIMES("Net.HttpJob.TotalTimeCached", total_time);
    } else  {
      UMA_HISTOGRAM_TIMES("Net.HttpJob.TotalTimeNotCached", total_time);
    }
  }

  static const bool use_overlapped_read_histogram =
      base::FieldTrialList::TrialExists("OverlappedReadImpact");
  if (use_overlapped_read_histogram) {
    UMA_HISTOGRAM_TIMES(
        base::FieldTrial::MakeName("Net.HttpJob.TotalTime",
                                   "OverlappedReadImpact"),
        total_time);

    if (reason == FINISHED) {
      UMA_HISTOGRAM_TIMES(
          base::FieldTrial::MakeName("Net.HttpJob.TotalTimeSuccess",
                                     "OverlappedReadImpact"),
          total_time);
    } else {
      UMA_HISTOGRAM_TIMES(
          base::FieldTrial::MakeName("Net.HttpJob.TotalTimeCancel",
                                     "OverlappedReadImpact"),
          total_time);
    }

    if (response_info_) {
      if (response_info_->was_cached) {
        UMA_HISTOGRAM_TIMES(
            base::FieldTrial::MakeName("Net.HttpJob.TotalTimeCached",
                                       "OverlappedReadImpact"),
            total_time);
      } else  {
        UMA_HISTOGRAM_TIMES(
            base::FieldTrial::MakeName("Net.HttpJob.TotalTimeNotCached",
                                       "OverlappedReadImpact"),
            total_time);
      }
    }
  }

  static const bool cache_sensitivity_analysis =
      base::FieldTrialList::TrialExists("CacheSensitivityAnalysis");
  if (cache_sensitivity_analysis) {
    UMA_HISTOGRAM_TIMES(
        base::FieldTrial::MakeName("Net.HttpJob.TotalTime",
                                   "CacheSensitivityAnalysis"),
        total_time);

    if (reason == FINISHED) {
      UMA_HISTOGRAM_TIMES(
          base::FieldTrial::MakeName("Net.HttpJob.TotalTimeSuccess",
                                     "CacheSensitivityAnalysis"),
          total_time);
    } else {
      UMA_HISTOGRAM_TIMES(
          base::FieldTrial::MakeName("Net.HttpJob.TotalTimeCancel",
                                     "CacheSensitivityAnalysis"),
          total_time);
    }

    if (response_info_) {
      if (response_info_->was_cached) {
        UMA_HISTOGRAM_TIMES(
            base::FieldTrial::MakeName("Net.HttpJob.TotalTimeCached",
                                       "CacheSensitivityAnalysis"),
            total_time);
      } else  {
        UMA_HISTOGRAM_TIMES(
            base::FieldTrial::MakeName("Net.HttpJob.TotalTimeNotCached",
                                       "CacheSensitivityAnalysis"),
            total_time);
      }
    }
  }

  start_time_ = base::TimeTicks();
}

void URLRequestHttpJob::DoneWithRequest(CompletionCause reason) {
  if (done_)
    return;
  done_ = true;
  RecordPerfHistograms(reason);
  if (reason == FINISHED) {
    request_->set_received_response_content_length(prefilter_bytes_read());
    RecordCompressionHistograms();
  }
}

HttpResponseHeaders* URLRequestHttpJob::GetResponseHeaders() const {
  DCHECK(transaction_.get());
  DCHECK(transaction_->GetResponseInfo());
  return override_response_headers_.get() ?
      override_response_headers_ :
      transaction_->GetResponseInfo()->headers;
}

void URLRequestHttpJob::NotifyURLRequestDestroyed() {
  awaiting_callback_ = false;
}

void URLRequestHttpJob::OnDetachRequest() {
  http_transaction_delegate_->OnDetachRequest();
}

}  // namespace net
