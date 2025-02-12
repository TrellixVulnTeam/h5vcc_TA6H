/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef WebFilterOperations_h
#define WebFilterOperations_h

#include "WebCommon.h"
#include "WebFilterOperation.h"
#include "WebPrivateOwnPtr.h"

namespace WebKit {

class WebFilterOperationsPrivate;

// An ordered list of filter operations.
class WEBKIT_EXPORT WebFilterOperations {
public:
    WebFilterOperations() { initialize(); }
    WebFilterOperations(const WebFilterOperations& other)
    {
        initialize();
        assign(other);
    }
    WebFilterOperations& operator=(const WebFilterOperations& other)
    {
        assign(other);
        return *this;
    }
    ~WebFilterOperations() { destroy(); }

    void assign(const WebFilterOperations&);
    bool equals(const WebFilterOperations&) const;

    void append(const WebFilterOperation&);

    // Removes all filter operations.
    void clear();
    bool isEmpty() const;

    void getOutsets(int& top, int& right, int& bottom, int& left) const;
    bool hasFilterThatMovesPixels() const;
    bool hasFilterThatAffectsOpacity() const;

    size_t size() const;
    WebFilterOperation at(size_t) const;

private:
    void initialize();
    void destroy();

    WebPrivateOwnPtr<WebFilterOperationsPrivate> m_private;
};

inline bool operator==(const WebFilterOperations& a, const WebFilterOperations& b)
{
    return a.equals(b);
}

inline bool operator!=(const WebFilterOperations& a, const WebFilterOperations& b)
{
    return !(a == b);
}

}

#endif
