/* Copyright (c) 2026 JC Technolabs
   License: GPL-3.0

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "cloud/connectorError.h"

namespace AIAssistant
{
    std::string_view Describe(ConnectorErrorCode code)
    {
        switch (code)
        {
            case ConnectorErrorCode::InvalidConfig:      return "invalid_config";
            case ConnectorErrorCode::InvalidEndpoint:    return "invalid_endpoint";
            case ConnectorErrorCode::CredentialMissing:  return "credential_missing";
            case ConnectorErrorCode::CredentialInvalid:  return "credential_invalid";
            case ConnectorErrorCode::OAuthError:         return "oauth_error";
            case ConnectorErrorCode::NetworkError:       return "network_error";
            case ConnectorErrorCode::AuthFailure:        return "auth_failure";
            case ConnectorErrorCode::HttpError:          return "http_error";
            case ConnectorErrorCode::ValueOutOfRange:    return "value_out_of_range";
            case ConnectorErrorCode::UnknownError:       return "unknown_error";
        }
        // Reachable only if a new ConnectorErrorCode is added without extending
        // the switch above.  -Wswitch catches it at compile time on most builds;
        // this fallback is the runtime backstop for the rest.
        return "unhandled_code";
    }

    bool IsConnectionFailure(ConnectorErrorCode code)
    {
        switch (code)
        {
            case ConnectorErrorCode::InvalidConfig:
            case ConnectorErrorCode::InvalidEndpoint:
            case ConnectorErrorCode::CredentialMissing:
            case ConnectorErrorCode::CredentialInvalid:
            case ConnectorErrorCode::OAuthError:
            case ConnectorErrorCode::NetworkError:
            case ConnectorErrorCode::AuthFailure:
            case ConnectorErrorCode::HttpError:
                return true;
            case ConnectorErrorCode::ValueOutOfRange:
                return false;
            case ConnectorErrorCode::UnknownError:
                // Conservative default — a bare `UnknownError` (no specific
                // classification by the emitter) is treated as connection-class
                // so a regression in code-tagging doesn't silently bypass the
                // breaker.  Better to spuriously open than to fail to detect a
                // real connection-health degradation.
                return true;
        }
        // Reachable only on a new variant — same -Wswitch posture as Describe.
        return true;
    }
} // namespace AIAssistant
