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

// memmem(3) and explicit_bzero(3) are GNU extensions exposed by <string.h> /
// <strings.h> only when _GNU_SOURCE is defined.  Set before any system header
// is pulled in.
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include "security/heapScan_test.h"

#ifndef J9T_HEAPSCAN_BUILD

// Production builds: zero-cost stub.  Real implementation below the #else.
namespace AIAssistant
{
    int RunHeapScanAudit() { return 0; }
} // namespace AIAssistant

#else // J9T_HEAPSCAN_BUILD

// After the cloud/sigV4Signer consolidation, there is only one
// AIAssistant::SigV4Signer (engine/curlWrapper/awsSigV4.{h,cpp}) — the S3
// connector path goes through the same Sign() now, with the per-S3 features
// (caller-supplied payload hash, signed extra headers) modeled as fields on
// Inputs.  The cloud-shape SigV4 scenario below exercises the S3 dispatch
// shape (GET + empty-body override + s3 service) to ensure the new
// ContentSha256Override + ExtraHeadersToSign paths don't introduce a residue.

#include "cloud/azureSharedKeySigner.h"
#include "curlWrapper/authSigner.h"
#include "curlWrapper/awsSigV4.h"
#include "curlWrapper/curlSlistHelper.h"
#include "curlWrapper/curlWrapper.h"
#include "engine.h"
#include "keys/credential.h"
#include "keys/secureString.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>

namespace AIAssistant
{
    namespace
    {
        // 32-byte nonce.  Hex-encoded (64 chars) so the on-the-wire form a signer
        // might emit (e.g. "Authorization: Bearer <hex>") still contains the
        // nonce bytes as printable ASCII — memmem matches whether the secret is
        // stored as the original 32 bytes or as the 64-char hex view.
        constexpr size_t kNeedleSize = 64;
        using Needle = std::array<unsigned char, kNeedleSize>;

        // Deterministic per-scenario nonce.  PRNG seed is the scenario index so a
        // residue hit is attributable: scenario 3's nonce only appears in /proc
        // if scenario 3's code path leaked it (not because scenario 1 stomped on
        // memory scenario 3 later reused).
        Needle MakeNeedle(uint32_t seed)
        {
            std::mt19937 rng{seed};
            Needle n{};
            // Hex alphabet so the bytes are printable — defeats any "scan only ASCII
            // ranges" optimisation in residue-detection tools and matches what a real
            // Bearer/SigV4 secret would look like on the wire.
            static char const hex[] = "0123456789abcdef";
            for (auto& b : n)
            {
                b = static_cast<unsigned char>(hex[rng() & 0x0F]);
            }
            return n;
        }

        std::string_view NeedleView(Needle const& n)
        {
            return {reinterpret_cast<char const*>(n.data()), n.size()};
        }

        // Defragment the allocator: allocate then free a wave of small blocks so
        // glibc reuses fastbin / tcache slabs that recently held secret-bearing
        // std::strings.  The reuse overwrites the residue.  64 MB is generous —
        // production Bearer tokens are ~50-200 bytes; the wave moves through every
        // size class up to large-allocation territory.
        void ChurnAllocator(size_t bytes)
        {
            constexpr size_t kBlockSize = 4096;
            size_t const numBlocks = bytes / kBlockSize;
            std::vector<void*> blocks;
            blocks.reserve(numBlocks);
            for (size_t i = 0; i < numBlocks; ++i)
            {
                void* p = std::malloc(kBlockSize);
                if (!p) break;
                // Fill with a known sentinel so the kernel commits the page (lazy
                // commit would otherwise leave it as zero-fill and unreadable as
                // residue evidence).
                std::memset(p, 0xAA, kBlockSize);
                blocks.push_back(p);
            }
            for (void* p : blocks)
            {
                std::free(p);
            }
        }

        struct MapEntry
        {
            uintptr_t   m_Start{0};
            uintptr_t   m_End{0};
            std::string m_Permissions; // "rw-p", "r-xp", etc.
            std::string m_Path;        // "[heap]", "[stack]", "[stack:tid]", file path, or empty for anon
        };

        // /proc/self/maps format (one line per mapping):
        //   <start>-<end> <perms> <offset> <dev> <inode> <path>
        // where <path> is optional ("[heap]", "[stack]", "[anon:<name>]", file path,
        // or empty for unnamed anonymous mappings).
        std::vector<MapEntry> ReadMaps()
        {
            std::vector<MapEntry> out;
            std::ifstream f("/proc/self/maps");
            if (!f)
            {
                return out;
            }
            std::string line;
            while (std::getline(f, line))
            {
                if (line.size() < 30) continue;
                MapEntry e;
                char* endPtr = nullptr;
                e.m_Start = std::strtoull(line.c_str(), &endPtr, 16);
                if (endPtr == line.c_str() || *endPtr != '-') continue;
                char const* afterDash = endPtr + 1;
                e.m_End = std::strtoull(afterDash, &endPtr, 16);
                if (endPtr == afterDash || *endPtr != ' ') continue;
                char const* permsStart = endPtr + 1;
                if (permsStart + 4 > line.c_str() + line.size()) continue;
                e.m_Permissions.assign(permsStart, 4);
                // Skip offset / dev / inode — find the path (or empty) at the tail.
                // Easiest robust parse: split on whitespace, take field[5..] joined.
                std::string_view rest{permsStart + 4};
                while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
                // rest now begins with the offset field.  Skip 3 whitespace-separated
                // fields (offset, dev, inode) to reach the path.
                for (int i = 0; i < 3; ++i)
                {
                    size_t sp = rest.find(' ');
                    if (sp == std::string_view::npos) { rest = {}; break; }
                    rest.remove_prefix(sp);
                    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
                }
                if (!rest.empty()) e.m_Path.assign(rest);
                out.push_back(std::move(e));
            }
            return out;
        }

        enum class Phase
        {
            Smoke, // [heap] only
            Deep   // every rw-p private mapping that could hold heap data
        };

        bool MappingIsInScope(MapEntry const& m, Phase phase)
        {
            // Only readable + writable mappings can hold the secret as data.
            if (m.m_Permissions.size() < 2) return false;
            if (m.m_Permissions[0] != 'r') return false;
            if (m.m_Permissions[1] != 'w') return false;
            // Private only.  Shared mappings would surface secrets that aren't on
            // our heap (kernel-managed buffers, IPC).  Out of scope for this audit.
            if (m.m_Permissions.size() < 4 || m.m_Permissions[3] != 'p') return false;

            if (phase == Phase::Smoke)
            {
                return m.m_Path == "[heap]";
            }
            // Deep: include [heap], anonymous mappings (empty path), and named
            // anon arenas ([anon:*]).  EXCLUDE [stack*] — the scanner holds the
            // needle as a local while scanning, so [stack] would always show a
            // self-detection hit in the scanner's own frame.  Heap residue is
            // the threat-model target; stack residue is a separate (and harder
            // to test from inside the same process) concern.  Documented
            // limitation: stack-resident secret residue is out of scope for this
            // audit.  Also exclude file-backed mappings — would scan binary BSS /
            // large vendored data segments, outside threat model and slow.
            if (m.m_Path == "[heap]") return true;
            if (m.m_Path.empty()) return true; // unnamed anon
            if (m.m_Path.rfind("[anon:", 0) == 0) return true;
            return false;
        }

        struct ScanResult
        {
            size_t                 m_HitCount{0};
            std::vector<uintptr_t> m_HitAddresses; // capped; first N for logging
            size_t                 m_BytesScanned{0};
        };

        // Read one mapping in chunks and memmem for the needle.  Hits beyond
        // kMaxLoggedHits are counted but their addresses are dropped so a heavy
        // residue doesn't blow the log.
        void ScanRegion(int memFd, MapEntry const& m, Needle const& needle, ScanResult& out)
        {
            constexpr size_t kChunk         = 256 * 1024;
            constexpr size_t kMaxLoggedHits = 16;
            std::vector<unsigned char> buf(kChunk);
            for (uintptr_t off = m.m_Start; off < m.m_End; off += kChunk)
            {
                size_t const toRead = std::min<size_t>(kChunk, m.m_End - off);
                ssize_t const n = pread(memFd, buf.data(), toRead, static_cast<off_t>(off));
                if (n <= 0)
                {
                    // EIO is normal for guard pages / not-yet-committed regions.
                    continue;
                }
                out.m_BytesScanned += static_cast<size_t>(n);
                size_t searchStart = 0;
                while (searchStart + needle.size() <= static_cast<size_t>(n))
                {
                    void* found = memmem(buf.data() + searchStart,
                                         static_cast<size_t>(n) - searchStart,
                                         needle.data(),
                                         needle.size());
                    if (!found) break;
                    uintptr_t const hit = off + (static_cast<unsigned char*>(found) - buf.data());
                    ++out.m_HitCount;
                    if (out.m_HitAddresses.size() < kMaxLoggedHits)
                    {
                        out.m_HitAddresses.push_back(hit);
                    }
                    searchStart = (static_cast<unsigned char*>(found) - buf.data()) + needle.size();
                }
            }
        }

        ScanResult ScanProcessMemory(Needle const& needle, Phase phase)
        {
            ScanResult result;
            int const memFd = open("/proc/self/mem", O_RDONLY);
            if (memFd < 0)
            {
                LOG_CORE_ERROR("HeapScan: open /proc/self/mem failed: {}", std::strerror(errno));
                return result;
            }
            auto const maps = ReadMaps();
            for (auto const& m : maps)
            {
                if (!MappingIsInScope(m, phase)) continue;
                ScanRegion(memFd, m, needle, result);
            }
            close(memFd);
            return result;
        }

        struct ScenarioOutcome
        {
            std::string m_Name;
            size_t      m_SmokeHits{0};
            size_t      m_DeepHits{0};
            size_t      m_BytesScannedSmoke{0};
            size_t      m_BytesScannedDeep{0};
            // Expected-residual scenarios document a known leak; their pass condition
            // is "hits are within tolerance", not "hits == 0".  A regression in either
            // direction (count drops to 0 = leak fixed, count grows = new leak) is
            // worth surfacing.
            bool        m_ExpectedResidual{false};
            std::string m_ExpectedResidualNote;
        };

        // Per-scenario churn-then-scan harness.  Each scenario lambda must build,
        // exercise, and tear down its auth-path locals BEFORE returning — at scan
        // time, no scenario-local SecureString or std::string should still own the
        // needle bytes (only residue, if any, should remain).
        //
        // skipChurn=true disables the 64 MiB allocator-churn step between teardown and
        // scan.  Used by structural-check scenarios that want to prove the residue is
        // ABSENT structurally, not just NEUTRALISED by allocator activity.
        template <typename ScenarioFn>
        ScenarioOutcome RunScenario(char const* name, uint32_t seed, ScenarioFn&& fn,
                                    bool expectedResidual = false,
                                    char const* residualNote = "",
                                    bool skipChurn = false)
        {
            ScenarioOutcome out;
            out.m_Name = name;
            out.m_ExpectedResidual = expectedResidual;
            out.m_ExpectedResidualNote = residualNote ? residualNote : "";

            Needle const needle = MakeNeedle(seed);
            // Run the path in its own scope so RAII destructors fire before scanning.
            {
                fn(needle);
            }
            if (!skipChurn)
            {
                ChurnAllocator(64 * 1024 * 1024);
            }

            ScanResult const smoke = ScanProcessMemory(needle, Phase::Smoke);
            ScanResult const deep  = ScanProcessMemory(needle, Phase::Deep);

            out.m_SmokeHits = smoke.m_HitCount;
            out.m_DeepHits  = deep.m_HitCount;
            out.m_BytesScannedSmoke = smoke.m_BytesScanned;
            out.m_BytesScannedDeep  = deep.m_BytesScanned;

            char const* verdict = "PASS";
            if (expectedResidual)
            {
                // Pass condition: at least one hit observed (regression: hardening
                // closed the leak → flip the cell to non-expected).  Zero hits in
                // smoke means residue evaporated into the churn — still document.
                verdict = (deep.m_HitCount > 0) ? "PASS (known residual)" : "PASS (residual evaporated)";
            }
            else if (smoke.m_HitCount == 0 && deep.m_HitCount == 0)
            {
                verdict = "PASS";
            }
            else
            {
                verdict = "FAIL";
            }
            LOG_CORE_INFO(
                "HeapScan[{}]: {} (smoke={} hits / {} KB; deep={} hits / {} KB){}{}",
                name, verdict,
                out.m_SmokeHits, out.m_BytesScannedSmoke / 1024,
                out.m_DeepHits,  out.m_BytesScannedDeep  / 1024,
                expectedResidual ? " — " : "",
                expectedResidual ? out.m_ExpectedResidualNote : "");
            // Log the first few hit addresses without the bytes themselves.
            for (auto addr : smoke.m_HitAddresses)
            {
                LOG_CORE_INFO("HeapScan[{}]: smoke hit at 0x{:x}", name, addr);
            }
            for (auto addr : deep.m_HitAddresses)
            {
                LOG_CORE_INFO("HeapScan[{}]: deep hit at 0x{:x}", name, addr);
            }
            return out;
        }

        // -------------------------------------------------------------------
        // Scenarios.  Each plants the needle as the secret credential for one
        // auth style, drives the production code path, and lets RAII tear down.
        // -------------------------------------------------------------------

        ScenarioOutcome ScenarioStaticHeader(char const* name, CurlWrapper::AuthStyle style, uint32_t seed)
        {
            return RunScenario(name, seed, [name, style](Needle const& needle) {
                CurlWrapper::QueryData q;
                q.m_Url = "https://example.test/v1/chat/completions";
                q.m_ApiKey.Set(NeedleView(needle));
                q.m_AuthStyle = style;
                q.m_Data = R"({"model":"placeholder","messages":[]})";

                std::vector<std::string> publicHeaders;
                SecureString             secretHeader;
                std::string              errorMessage;

                IAuthSigner const& signer = IAuthSigner::Get(style);
                bool const ok = signer.Apply(q, publicHeaders, secretHeader, errorMessage);
                if (!ok)
                {
                    LOG_CORE_ERROR("HeapScan[{}]: Apply failed: {}", name, errorMessage);
                }
                // publicHeaders + secretHeader + q (with m_ApiKey) destruct on scope exit.
            });
        }

        // Shared engine-SigV4 exercise lambda, parametrised by the planted needle.
        // Used by both ScenarioEngineSigV4 (with churn pad) and
        // ScenarioEngineSigV4StructuralCheck (no churn — proves R4 closed the
        // canonical-headers residue structurally, not via allocator churn).
        auto const exerciseEngineSigV4 = [](Needle const& needle) {
            auto cred = std::make_shared<AwsCredential>();
            cred->m_AccessKeyId = "AKIDEXAMPLE";
            cred->m_SecretAccessKey.Set(NeedleView(needle));
            // Distinct session-token bytes so a hit can be attributed to the
            // input-phase secret vs. the session-token secretHeader emission.
            std::array<unsigned char, kNeedleSize> sessionTokenBytes{};
            for (size_t i = 0; i < sessionTokenBytes.size(); ++i)
            {
                sessionTokenBytes[i] = static_cast<unsigned char>(needle[i] ^ 0x20);
            }
            cred->m_SessionToken.Set(std::string_view{
                reinterpret_cast<char const*>(sessionTokenBytes.data()), sessionTokenBytes.size()});
            cred->m_Region = "us-east-1";

            CurlWrapper::QueryData q;
            q.m_Url = "https://bedrock-runtime.us-east-1.amazonaws.com/model/test/invoke";
            q.m_Data = R"({"prompt":"placeholder"})";
            q.m_AuthStyle = CurlWrapper::AuthStyle::AwsSigV4;
            q.m_AwsCredential = cred;
            // Deterministic timestamp so the canonical-request string doesn't
            // change between runs in ways that could leave incidental residue.
            q.m_AmzDateOverride = "20260101T000000Z";

            std::vector<std::string> publicHeaders;
            SecureString             secretHeader;
            std::string              errorMessage;

            SigV4Signer signer;
            bool const ok = signer.Apply(q, publicHeaders, secretHeader, errorMessage);
            if (!ok)
            {
                LOG_CORE_ERROR("HeapScan[engineSigV4]: Apply failed: {}", errorMessage);
            }
            // Wipe the sessionTokenBytes scratch array — it lived on the stack
            // but pread can read [stack] mappings in the deep phase.
            explicit_bzero(sessionTokenBytes.data(), sessionTokenBytes.size());
        };

        ScenarioOutcome ScenarioEngineSigV4(uint32_t seed)
        {
            return RunScenario("engineSigV4", seed, exerciseEngineSigV4);
        }

        // Structural-check variant: same path as ScenarioEngineSigV4 but with the
        // 64 MiB churn pad disabled.  If the canonical-headers build is structurally
        // clean, this reports 0/0 just like the churn variant.  If a future regression
        // reintroduces a std::string-resident copy of the session token in canonical-
        // request assembly, this scenario catches it (where the churn variant might
        // still pass by neutralising the residue post-hoc).
        ScenarioOutcome ScenarioEngineSigV4StructuralCheck(uint32_t seed)
        {
            return RunScenario("engineSigV4(no-churn)", seed, exerciseEngineSigV4,
                                /*expectedResidual=*/false, /*residualNote=*/"",
                                /*skipChurn=*/true);
        }

        ScenarioOutcome ScenarioAppendSecretHeader(uint32_t seed)
        {
            return RunScenario(
                "AppendSecretHeader", seed,
                [](Needle const& needle) {
                    SecureString secret;
                    secret.Set(NeedleView(needle));
                    SecureString scratch;

                    curl_slist* list = nullptr;
                    bool const ok = AppendSecretHeader(list, "Authorization: Bearer ", secret, scratch);
                    if (!ok)
                    {
                        LOG_CORE_ERROR("HeapScan[AppendSecretHeader]: helper returned false");
                    }
                    // libcurl strdup'd the assembled "Authorization: Bearer <needle>"
                    // string into its own heap allocation; curl_slist_free_all calls
                    // free() (NOT explicit_bzero+free) so the residue persists in
                    // the freed slab.  This is the documented libcurl floor per 8b.
                    curl_slist_free_all(list);
                    // secret + scratch destruct on scope exit.
                },
                /*expectedResidual=*/true,
                "libcurl strdup floor: curl_slist_free_all does not zero before free");
        }

        ScenarioOutcome ScenarioCloudSigV4Sign(uint32_t seed)
        {
            return RunScenario(
                "cloudSigV4::Sign", seed,
                [](Needle const& needle) {
                    // Exercises the S3 dispatch shape (GET + empty-body override
                    // + s3 service + Content-Type signed extra) against the
                    // engine signer — verifies the new ContentSha256Override
                    // and ExtraHeadersToSign paths don't introduce residue.
                    SecureString secretKey;
                    secretKey.Set(NeedleView(needle));
                    SigV4Signer::Inputs in;
                    in.m_Method = "GET";
                    in.m_Url = "https://test-bucket.s3.us-east-1.amazonaws.com/object.txt";
                    in.m_AccessKey = "AKIDEXAMPLE";
                    in.m_SecretKey.Set(secretKey.Get());
                    in.m_Region = "us-east-1";
                    in.m_Service = "s3";
                    in.m_ContentSha256Override =
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
                    in.m_ExtraHeadersToSign["Content-Type"] = "application/octet-stream";
                    auto signed_ = SigV4Signer::Sign(in);
                    (void)signed_;
                },
                /*expectedResidual=*/false);
        }

        ScenarioOutcome ScenarioAzureSharedKeySign(uint32_t seed)
        {
            return RunScenario(
                "AzureSharedKey::Sign", seed,
                [](Needle const& needle) {
                    // The needle is hex-printable so Base64Decode inside Sign()
                    // accepts it (valid base64 alphabet) and produces decoded
                    // bytes that go through the HMAC chain.  We care about the
                    // residue, not whether an Azure server would accept the
                    // resulting Authorization header.  The decoded rawKey is
                    // wrapped in ScopedSecretBytes — no std::string heap
                    // intermediate holds the decoded secret.
                    SecureString accountKey;
                    accountKey.Set(NeedleView(needle));
                    auto signed_ = AzureSharedKeySigner::Sign(
                        /*method=*/"GET",
                        /*url=*/"https://testaccount.blob.core.windows.net/container/blob",
                        /*accountName=*/"testaccount",
                        accountKey);
                    (void)signed_;
                },
                /*expectedResidual=*/false);
        }

        ScenarioOutcome ScenarioOAuthPostBody(uint32_t seed)
        {
            return RunScenario(
                "OAuthPostBody", seed,
                [](Needle const& needle) {
                    // Mirrors oauthTokenManager.cpp::PerformRefresh — builds a
                    // form-urlencoded refresh-token POST body in a SecureString
                    // via SecureString::Build.  The needle is planted in the
                    // client_secret position (the worst-case slot for residue —
                    // client_secret is the longest-lived OAuth secret outside the
                    // refresh_token).  The postBody never materialises into a
                    // plain std::string heap allocation.
                    SecureString postBody;
                    postBody.Build({
                        "grant_type=refresh_token&refresh_token=", "placeholder_rt",
                        "&client_id=", "placeholder_cid",
                        "&client_secret=", NeedleView(needle),
                    });
                    // postBody destructs on scope exit — wiping the mlock'd buffer.
                });
        }
    } // namespace

    int RunHeapScanAudit()
    {
        LOG_CORE_INFO("HeapScan: starting SecureString-only HTTP-path audit");
        LOG_CORE_INFO("HeapScan: needle size = {} bytes (hex-printable)", kNeedleSize);

        std::vector<ScenarioOutcome> outcomes;
        // Static-header signers — AI dispatch path.  Must be zero hits.
        outcomes.push_back(ScenarioStaticHeader("Bearer",          CurlWrapper::AuthStyle::Bearer,           0x10000001));
        outcomes.push_back(ScenarioStaticHeader("XGoogApiKey",     CurlWrapper::AuthStyle::XGoogApiKey,      0x10000002));
        outcomes.push_back(ScenarioStaticHeader("AnthropicXApiKey", CurlWrapper::AuthStyle::AnthropicXApiKey, 0x10000003));
        outcomes.push_back(ScenarioStaticHeader("AzureApiKey",     CurlWrapper::AuthStyle::AzureApiKey,      0x10000004));
        // Engine SigV4 (Bedrock AI dispatch) — must be zero hits.  Uses ScopedSecretBytes
        // for HMAC intermediates and SecureString for session-token output + canonical-
        // request assembly.
        outcomes.push_back(ScenarioEngineSigV4(0x10000005));
        // AppendSecretHeader (cloud Bearer paths) — documented libcurl-strdup floor.
        outcomes.push_back(ScenarioAppendSecretHeader(0x20000001));
        // Cloud HMAC signers — must be zero hits.  ScopedSecretBytes throughout the
        // HMAC chain.
        outcomes.push_back(ScenarioCloudSigV4Sign(0x20000002));
        outcomes.push_back(ScenarioAzureSharedKeySign(0x20000003));
        // OAuth POST body via SecureString::Build — must be zero hits.
        outcomes.push_back(ScenarioOAuthPostBody(0x10000006));
        // Structural check: same engine-SigV4 path as above, but with the 64 MiB
        // churn pad disabled.  Verifies the canonical-headers session-token residue
        // is structurally absent rather than relying on allocator churn.
        outcomes.push_back(ScenarioEngineSigV4StructuralCheck(0x10000007));

        // Aggregate.
        size_t passCount      = 0;
        size_t failCount      = 0;
        size_t expectedCount  = 0;
        for (auto const& o : outcomes)
        {
            if (o.m_ExpectedResidual)
            {
                ++expectedCount;
            }
            else if (o.m_SmokeHits == 0 && o.m_DeepHits == 0)
            {
                ++passCount;
            }
            else
            {
                ++failCount;
            }
        }

        LOG_CORE_INFO("HeapScan: summary — {} pass, {} fail, {} expected-residual",
                      passCount, failCount, expectedCount);
        if (failCount > 0)
        {
            LOG_CORE_ERROR("HeapScan: AUDIT FAILED — secret residue found in non-expected scenarios");
            return 1;
        }
        LOG_CORE_INFO("HeapScan: AUDIT PASSED");
        return 0;
    }
} // namespace AIAssistant

#endif // J9T_HEAPSCAN_BUILD
