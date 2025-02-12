// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SAFE_BROWSING_PROTOCOL_MANAGER_H_
#define CHROME_BROWSER_SAFE_BROWSING_PROTOCOL_MANAGER_H_

// A class that implements Chrome's interface with the SafeBrowsing protocol.
// The SafeBrowsingProtocolManager handles formatting and making requests of,
// and handling responses from, Google's SafeBrowsing servers. This class uses
// The SafeBrowsingProtocolParser class to do the actual parsing.

#include <deque>
#include <set>
#include <string>
#include <vector>

#include "base/gtest_prod_util.h"
#include "base/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "base/threading/non_thread_safe.h"
#include "base/time.h"
#include "base/timer.h"
#include "chrome/browser/safe_browsing/chunk_range.h"
#include "chrome/browser/safe_browsing/protocol_parser.h"
#include "chrome/browser/safe_browsing/protocol_manager_helper.h"
#include "chrome/browser/safe_browsing/safe_browsing_util.h"
#include "googleurl/src/gurl.h"
#include "net/url_request/url_fetcher_delegate.h"

namespace net {
class URLFetcher;
class URLRequestContextGetter;
}  // namespace net

#if defined(COMPILER_GCC)
// Allows us to use URLFetchers in a hash_map with gcc (MSVC is okay without
// specifying this).
namespace BASE_HASH_NAMESPACE {
template<>
struct hash<const net::URLFetcher*> {
  size_t operator()(const net::URLFetcher* fetcher) const {
    return reinterpret_cast<size_t>(fetcher);
  }
};
}
#endif

class SBProtocolManagerFactory;
class SafeBrowsingProtocolManagerDelegate;

class SafeBrowsingProtocolManager : public net::URLFetcherDelegate,
                                    public base::NonThreadSafe {
 public:
  // FullHashCallback is invoked when GetFullHash completes.
  // Parameters:
  //   - The vector of full hash results. If empty, indicates that there
  //     were no matches, and that the resource is safe.
  //   - Whether the result can be cached. This may not be the case when
  //     the result did not come from the SB server, for example.
  typedef base::Callback<void(const std::vector<SBFullHashResult>&,
                              bool)> FullHashCallback;

  virtual ~SafeBrowsingProtocolManager();

  // Makes the passed |factory| the factory used to instantiate
  // a SafeBrowsingService. Useful for tests.
  static void RegisterFactory(SBProtocolManagerFactory* factory) {
    factory_ = factory;
  }

  // Create an instance of the safe browsing protocol manager.
  static SafeBrowsingProtocolManager* Create(
      SafeBrowsingProtocolManagerDelegate* delegate,
      net::URLRequestContextGetter* request_context_getter,
      const SafeBrowsingProtocolConfig& config);

  // Sets up the update schedule and internal state for making periodic requests
  // of the Safebrowsing servers.
  virtual void Initialize();

  // net::URLFetcherDelegate interface.
  virtual void OnURLFetchComplete(const net::URLFetcher* source) OVERRIDE;

  // Retrieve the full hash for a set of prefixes, and invoke the callback
  // argument when the results are retrieved. The callback may be invoked
  // synchronously.
  virtual void GetFullHash(const std::vector<SBPrefix>& prefixes,
                           FullHashCallback callback,
                           bool is_download);

  // Forces the start of next update after |interval| time.
  void ForceScheduleNextUpdate(base::TimeDelta interval);

  // Scheduled update callback.
  void GetNextUpdate();

  // Called by the SafeBrowsingService when our request for a list of all chunks
  // for each list is done.  If database_error is true, that means the protocol
  // manager shouldn't fetch updates since they can't be written to disk.  It
  // should try again later to open the database.
  void OnGetChunksComplete(const std::vector<SBListChunkRanges>& list,
                           bool database_error);

  // Called after the chunks that were parsed were inserted in the database.
  void OnChunkInserted();

  // The last time we received an update.
  base::Time last_update() const { return last_update_; }

  // Setter for additional_query_. To make sure the additional_query_ won't
  // be changed in the middle of an update, caller (e.g.: SafeBrowsingService)
  // should call this after callbacks triggered in UpdateFinished() or before
  // IssueUpdateRequest().
  void set_additional_query(const std::string& query) {
    additional_query_ = query;
  }
  const std::string& additional_query() const {
    return additional_query_;
  }

  // Enumerate failures for histogramming purposes.  DO NOT CHANGE THE
  // ORDERING OF THESE VALUES.
  enum ResultType {
    // 200 response code means that the server recognized the hash
    // prefix, while 204 is an empty response indicating that the
    // server did not recognize it.
    GET_HASH_STATUS_200,
    GET_HASH_STATUS_204,

    // Subset of successful responses which returned no full hashes.
    // This includes the 204 case, and also 200 responses for stale
    // prefixes (deleted at the server but yet deleted on the client).
    GET_HASH_FULL_HASH_EMPTY,

    // Subset of successful responses for which one or more of the
    // full hashes matched (should lead to an interstitial).
    GET_HASH_FULL_HASH_HIT,

    // Subset of successful responses which weren't empty and have no
    // matches.  It means that there was a prefix collision which was
    // cleared up by the full hashes.
    GET_HASH_FULL_HASH_MISS,

    // Memory space for histograms is determined by the max.  ALWAYS
    // ADD NEW VALUES BEFORE THIS ONE.
    GET_HASH_RESULT_MAX
  };

  // Record a GetHash result. |is_download| indicates if the get
  // hash is triggered by download related lookup.
  static void RecordGetHashResult(bool is_download,
                                  ResultType result_type);

 protected:
  // Constructs a SafeBrowsingProtocolManager for |delegate| that issues
  // network requests using |request_context_getter|.
  SafeBrowsingProtocolManager(
      SafeBrowsingProtocolManagerDelegate* delegate,
      net::URLRequestContextGetter* request_context_getter,
      const SafeBrowsingProtocolConfig& config);

 private:
  FRIEND_TEST_ALL_PREFIXES(SafeBrowsingProtocolManagerTest, TestBackOffTimes);
  FRIEND_TEST_ALL_PREFIXES(SafeBrowsingProtocolManagerTest, TestChunkStrings);
  FRIEND_TEST_ALL_PREFIXES(SafeBrowsingProtocolManagerTest, TestGetHashUrl);
  FRIEND_TEST_ALL_PREFIXES(SafeBrowsingProtocolManagerTest,
                           TestGetHashBackOffTimes);
  FRIEND_TEST_ALL_PREFIXES(SafeBrowsingProtocolManagerTest, TestNextChunkUrl);
  FRIEND_TEST_ALL_PREFIXES(SafeBrowsingProtocolManagerTest, TestUpdateUrl);
  friend class SafeBrowsingServerTest;
  friend class SBProtocolManagerFactoryImpl;

  // Internal API for fetching information from the SafeBrowsing servers. The
  // GetHash requests are higher priority since they can block user requests
  // so are handled separately.
  enum SafeBrowsingRequestType {
    NO_REQUEST = 0,     // No requests in progress
    UPDATE_REQUEST,     // Request for redirect URLs
    CHUNK_REQUEST,      // Request for a specific chunk
  };

  // Generates Update URL for querying about the latest set of chunk updates.
  GURL UpdateUrl() const;
  // Generates GetHash request URL for retrieving full hashes.
  GURL GetHashUrl() const;
  // Generates URL for reporting safe browsing hits for UMA users.

  // Composes a ChunkUrl based on input string.
  GURL NextChunkUrl(const std::string& input) const;

  // Returns the time for the next update request. If |back_off| is true,
  // the time returned will increment an error count and return the appriate
  // next time (see ScheduleNextUpdate below).
  base::TimeDelta GetNextUpdateInterval(bool back_off);

  // Worker function for calculating GetHash and Update backoff times (in
  // seconds). |multiplier| is doubled for each consecutive error between the
  // 2nd and 5th, and |error_count| is incremented with each call.
  base::TimeDelta GetNextBackOffInterval(int* error_count,
                                         int* multiplier) const;

  // Manages our update with the next allowable update time. If 'back_off_' is
  // true, we must decrease the frequency of requests of the SafeBrowsing
  // service according to section 5 of the protocol specification.
  // When disable_auto_update_ is set, ScheduleNextUpdate will do nothing.
  // ForceScheduleNextUpdate has to be called to trigger the update.
  void ScheduleNextUpdate(bool back_off);

  // Sends a request for a list of chunks we should download to the SafeBrowsing
  // servers. In order to format this request, we need to send all the chunk
  // numbers for each list that we have to the server. Getting the chunk numbers
  // requires a database query (run on the database thread), and the request
  // is sent upon completion of that query in OnGetChunksComplete.
  void IssueUpdateRequest();

  // Sends a request for a chunk to the SafeBrowsing servers.
  void IssueChunkRequest();

  // Formats a string returned from the database into:
  //   "list_name;a:<add_chunk_ranges>:s:<sub_chunk_ranges>\n"
  static std::string FormatList(const SBListChunkRanges& list);

  // Runs the protocol parser on received data and update the
  // SafeBrowsingService with the new content. Returns 'true' on successful
  // parse, 'false' on error.
  bool HandleServiceResponse(const GURL& url, const char* data, int length);

  // Updates internal state for each GetHash response error, assuming that the
  // current time is |now|.
  void HandleGetHashError(const base::Time& now);

  // Helper function for update completion.
  void UpdateFinished(bool success);

  // A callback that runs if we timeout waiting for a response to an update
  // request. We use this to properly set our update state.
  void UpdateResponseTimeout();

 private:
  // Map of GetHash requests to parameters which created it.
  struct FullHashDetails {
    FullHashDetails();
    FullHashDetails(FullHashCallback callback, bool is_download);
    ~FullHashDetails();

    FullHashCallback callback;
    bool is_download;
  };
  typedef base::hash_map<const net::URLFetcher*, FullHashDetails> HashRequests;

  // The factory that controls the creation of SafeBrowsingProtocolManager.
  // This is used by tests.
  static SBProtocolManagerFactory* factory_;

  // Our delegate.
  SafeBrowsingProtocolManagerDelegate* delegate_;

  // Current active request (in case we need to cancel) for updates or chunks
  // from the SafeBrowsing service. We can only have one of these outstanding
  // at any given time unlike GetHash requests, which are tracked separately.
  scoped_ptr<net::URLFetcher> request_;

  // The kind of request that is currently in progress.
  SafeBrowsingRequestType request_type_;

  // The number of HTTP response errors, used for request backoff timing.
  int update_error_count_;
  int gethash_error_count_;

  // Multipliers which double (max == 8) for each error after the second.
  int update_back_off_mult_;
  int gethash_back_off_mult_;

  // Multiplier between 0 and 1 to spread clients over an interval.
  float back_off_fuzz_;

  // The list for which we are make a request.
  std::string list_name_;

  // For managing the next earliest time to query the SafeBrowsing servers for
  // updates.
  base::TimeDelta next_update_interval_;
  base::OneShotTimer<SafeBrowsingProtocolManager> update_timer_;

  // All chunk requests that need to be made.
  std::deque<ChunkUrl> chunk_request_urls_;

  HashRequests hash_requests_;

  // The next scheduled update has special behavior for the first 2 requests.
  enum UpdateRequestState {
    FIRST_REQUEST = 0,
    SECOND_REQUEST,
    NORMAL_REQUEST
  };
  UpdateRequestState update_state_;

  // True if the service has been given an add/sub chunk but it hasn't been
  // added to the database yet.
  bool chunk_pending_to_write_;

  // The last time we successfully received an update.
  base::Time last_update_;

  // While in GetHash backoff, we can't make another GetHash until this time.
  base::Time next_gethash_time_;

  // Current product version sent in each request.
  std::string version_;

  // Used for measuring chunk request latency.
  base::Time chunk_request_start_;

  // Tracks the size of each update (in bytes).
  int update_size_;

  // The safe browsing client name sent in each request.
  std::string client_name_;

  // A string that is appended to the end of URLs for download, gethash,
  // safebrowsing hits and chunk update requests.
  std::string additional_query_;

  // The context we use to issue network requests.
  scoped_refptr<net::URLRequestContextGetter> request_context_getter_;

  // URL prefix where browser fetches safebrowsing chunk updates, and hashes.
  std::string url_prefix_;

  // When true, protocol manager will not start an update unless
  // ForceScheduleNextUpdate() is called. This is set for testing purpose.
  bool disable_auto_update_;

  // ID for URLFetchers for testing.
  int url_fetcher_id_;

  DISALLOW_COPY_AND_ASSIGN(SafeBrowsingProtocolManager);
};

// Interface of a factory to create ProtocolManager.  Useful for tests.
class SBProtocolManagerFactory {
 public:
  SBProtocolManagerFactory() {}
  virtual ~SBProtocolManagerFactory() {}
  virtual SafeBrowsingProtocolManager* CreateProtocolManager(
      SafeBrowsingProtocolManagerDelegate* delegate,
      net::URLRequestContextGetter* request_context_getter,
      const SafeBrowsingProtocolConfig& config) = 0;
 private:
  DISALLOW_COPY_AND_ASSIGN(SBProtocolManagerFactory);
};

// Delegate interface for the SafeBrowsingProtocolManager.
class SafeBrowsingProtocolManagerDelegate {
 public:
  typedef base::Callback<void(const std::vector<SBListChunkRanges>&, bool)>
      GetChunksCallback;

  virtual ~SafeBrowsingProtocolManagerDelegate();

  // |UpdateStarted()| is called just before the SafeBrowsing update protocol
  // has begun.
  virtual void UpdateStarted() = 0;

  // |UpdateFinished()| is called just after the SafeBrowsing update protocol
  // has completed.
  virtual void UpdateFinished(bool success) = 0;

  // Wipe out the local database. The SafeBrowsing server can request this.
  virtual void ResetDatabase() = 0;

  // Retrieve all the local database chunks, and invoke |callback| with the
  // results. The SafeBrowsingProtocolManagerDelegate must only invoke the
  // callback if the SafeBrowsingProtocolManager is still alive. Only one call
  // may be made to GetChunks at a time.
  virtual void GetChunks(GetChunksCallback callback) = 0;

  // Add new chunks to the database.
  virtual void AddChunks(const std::string& list, SBChunkList* chunks) = 0;

  // Delete chunks from the database.
  virtual void DeleteChunks(
      std::vector<SBChunkDelete>* delete_chunks) = 0;
};

#endif  // CHROME_BROWSER_SAFE_BROWSING_PROTOCOL_MANAGER_H_
