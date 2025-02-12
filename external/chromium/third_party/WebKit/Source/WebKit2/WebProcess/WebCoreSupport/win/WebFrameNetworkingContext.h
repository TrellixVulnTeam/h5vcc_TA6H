/*
    Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#ifndef WebFrameNetworkingContext_h
#define WebFrameNetworkingContext_h

#include "WebFrame.h"

#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/NotImplemented.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>

class WebFrameNetworkingContext : public WebCore::FrameNetworkingContext {
public:
    static PassRefPtr<WebFrameNetworkingContext> create(WebKit::WebFrame* frame)
    {
        return adoptRef(new WebFrameNetworkingContext(frame));
    }

private:
    WebFrameNetworkingContext(WebKit::WebFrame* frame)
        : WebCore::FrameNetworkingContext(frame->coreFrame())
    {
    }

    virtual String userAgent() const;
    virtual String referrer() const;
    virtual WebCore::ResourceError blockedError(const WebCore::ResourceRequest&) const;

#if USE(CFNETWORK)
    virtual bool inPrivateBrowsingMode() const;
    virtual CFURLStorageSessionRef storageSession() const;
#endif

    String m_userAgent;
};

#endif // WebFrameNetworkingContext_h
