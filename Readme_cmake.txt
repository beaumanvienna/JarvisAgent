cmake .. \                                                                                                                                                                                                               
-DBUILD_SHARED_LIBS=OFF \
-DCURL_USE_OPENSSL=ON \
-DUSE_NGHTTP2=ON \
-DCURL_USE_LIBPSL=OFF -DCMAKE_VERBOSE_MAKEFILE=ON -DHAVE_BROTLI=OFF
-- Using CMake version 4.1.2
-- curl version=[8.17.0-DEV]
-- CMake platform flags: UNIX GCC
-- Picky compiler options: -Werror-implicit-function-declaration -W -Wall -pedantic -Wbad-function-cast -Wconversion -Wmissing-declarations -Wmissing-prototypes -Wnested-externs -Wno-long-long -Wno-multichar -Wpointer-arith -Wshadow -Wsign-compare -Wundef -Wunused -Wwrite-strings -Waddress -Wattributes -Wcast-align -Wcast-qual -Wdeclaration-after-statement -Wdiv-by-zero -Wempty-body -Wendif-labels -Wfloat-equal -Wformat-security -Wignored-qualifiers -Wmissing-field-initializers -Wmissing-noreturn -Wno-format-nonliteral -Wno-padded -Wno-sign-conversion -Wno-switch-default -Wno-switch-enum -Wno-system-headers -Wold-style-definition -Wredundant-decls -Wstrict-prototypes -Wtype-limits -Wunreachable-code -Wunused-parameter -Wvla -Wclobbered -Wmissing-parameter-type -Wold-style-declaration -Wpragmas -Wstrict-aliasing=3 -ftree-vrp -Wjump-misses-init -Wdouble-promotion -Wformat=2 -Wtrampolines -Warray-bounds=2 -Wduplicated-cond -Wnull-dereference -fdelete-null-pointer-checks -Wshift-negative-value -Wshift-overflow=2 -Wunused-const-variable -Walloc-zero -Wduplicated-branches -Wformat-truncation=2 -Wimplicit-fallthrough -Wrestrict -Warith-conversion -Wenum-conversion -Warray-compare -Wenum-int-mismatch -Wxor-used-as-pow
-- Could NOT find Zstd (missing: ZSTD_INCLUDE_DIR ZSTD_LIBRARY) 
-- Could NOT find NGHTTP2 (missing: NGHTTP2_INCLUDE_DIR NGHTTP2_LIBRARY) 
-- Could NOT find Libidn2 (missing: LIBIDN2_INCLUDE_DIR LIBIDN2_LIBRARY) 
-- Could NOT find Libssh2 (missing: LIBSSH2_INCLUDE_DIR LIBSSH2_LIBRARY) 
-- Enabled examples with dependencies: cacertinmem:USE_OPENSSL multithread:HAVE_PTHREAD_H threaded-ssl:HAVE_PTHREAD_H usercertinmem:USE_OPENSSL
-- Protocols: dict file ftp ftps gopher gophers http https imap imaps ipfs ipns mqtt pop3 pop3s rtsp smb smbs smtp smtps telnet tftp ws wss
-- Features: alt-svc AsynchDNS HSTS HTTPS-proxy IPv6 Largefile libz NTLM SSL threadsafe TLS-SRP UnixSockets
-- Enabled SSL backends: OpenSSL v3+
-- Configuring done (0.1s)
-- Generating done (0.2s)


this generates `./lib/curl_config.h` which was copied to `vendor/curl/lib/curl_config.h` and is used in the premake5(!) build
