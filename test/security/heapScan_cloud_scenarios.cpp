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

// Isolated TU for the cloud-side SigV4Signer + AzureSharedKeySigner scenarios.
// Two distinct classes named AIAssistant::SigV4Signer exist — one in
// engine/curlWrapper/awsSigV4.{h,cpp} (Bedrock AI-dispatch path) and one in
// application/cloud/sigV4Signer.{h,cpp} (S3 cloud connector path).  The two
// headers must not be included in the same TU.  The main heapScan_test.cpp
// pulls in the engine signer for ScenarioEngineSigV4; the cloud signer
// scenarios live here.

#ifdef J9T_HEAPSCAN_BUILD

    #include "cloud/azureSharedKeySigner.h"
    #include "cloud/sigV4Signer.h"
    #include "keys/secureString.h"

namespace AIAssistant
{
    namespace HeapScanCloud
    {
        // Exercises application/cloud/sigV4Signer.cpp::Sign() with the given
        // SecureString secret bytes.  The signer's HMAC chain is wrapped in
        // ScopedSecretBytes — no std::string heap intermediate holds the secret.
        void ExerciseCloudSigV4Sign(SecureString const& secretKey)
        {
            auto signed_ = SigV4Signer::Sign(
                /*method=*/"GET",
                /*url=*/"https://test-bucket.s3.us-east-1.amazonaws.com/object.txt",
                /*region=*/"us-east-1",
                /*service=*/"s3",
                /*accessKeyId=*/"AKIDEXAMPLE",
                secretKey,
                /*payloadHash=*/SigV4Signer::EmptyPayloadHash());
            (void)signed_;
        }

        // Exercises application/cloud/azureSharedKeySigner.cpp::Sign().  The
        // signer's Base64Decode writes into a ScopedSecretBytes-wrapped byte
        // vector — no std::string heap intermediate holds the decoded key.
        void ExerciseAzureSharedKeySign(SecureString const& accountKey)
        {
            auto signed_ = AzureSharedKeySigner::Sign(
                /*method=*/"GET",
                /*url=*/"https://testaccount.blob.core.windows.net/container/blob",
                /*accountName=*/"testaccount",
                accountKey);
            (void)signed_;
        }
    } // namespace HeapScanCloud
} // namespace AIAssistant

#endif // J9T_HEAPSCAN_BUILD
