-- copyright JC Techno labs 2025

project "ssl"
    kind "StaticLib"
    language "C"
    
    targetdir "bin/%{cfg.buildcfg}"
    objdir ("bin-int/%{cfg.buildcfg}")

    defines
    {
		"OPENSSL_USE_NODELETE",
		"L_ENDIAN",
		"OPENSSL_PIC",
		"OPENSSL_BUILDING_OPENSSL",
		"OPENSSLDIR=\"\"",
		"ENGINESDIR=\"\"",
		"MODULESDIR=\"\""
    }
    
    includedirs
    {
		".",
        "include"
    }

    files
    {
		"ssl/bio_ssl.c",
		"ssl/d1_lib.c",
		"ssl/d1_msg.c",
		"ssl/d1_srtp.c",
		"ssl/methods.c",
		"ssl/pqueue.c",
		"ssl/priority_queue.c",
		"ssl/s3_enc.c",
		"ssl/s3_lib.c",
		"ssl/s3_msg.c",
		"ssl/ssl_asn1.c",
		"ssl/ssl_cert.c",
		"ssl/ssl_cert_comp.c",
		"ssl/ssl_ciph.c",
		"ssl/ssl_conf.c",
		"ssl/ssl_err_legacy.c",
		"ssl/ssl_init.c",
		"ssl/ssl_lib.c",
		"ssl/ssl_mcnf.c",
		"ssl/ssl_rsa.c",
		"ssl/ssl_rsa_legacy.c",
		"ssl/ssl_sess.c",
		"ssl/ssl_stat.c",
		"ssl/ssl_txt.c",
		"ssl/ssl_utst.c",
		"ssl/t1_enc.c",
		"ssl/t1_lib.c",
		"ssl/t1_trce.c",
		"ssl/tls13_enc.c",
		"ssl/tls_depr.c",
		"ssl/tls_srp.c",
		"ssl/quic/cc_newreno.c",
		"ssl/quic/json_enc.c",
		"ssl/quic/qlog.c",
		"ssl/quic/qlog_event_helpers.c",
		"ssl/quic/quic_ackm.c",
		"ssl/quic/quic_cfq.c",
		"ssl/quic/quic_channel.c",
		"ssl/quic/quic_demux.c",
		"ssl/quic/quic_engine.c",
		"ssl/quic/quic_fc.c",
		"ssl/quic/quic_fifd.c",
		"ssl/quic/quic_impl.c",
		"ssl/quic/quic_lcidm.c",
		"ssl/quic/quic_method.c",
		"ssl/quic/quic_obj.c",
		"ssl/quic/quic_port.c",
		"ssl/quic/quic_rcidm.c",
		"ssl/quic/quic_reactor.c",
		"ssl/quic/quic_reactor_wait_ctx.c",
		"ssl/quic/quic_record_rx.c",
		"ssl/quic/quic_record_shared.c",
		"ssl/quic/quic_record_tx.c",
		"ssl/quic/quic_record_util.c",
		"ssl/quic/quic_rstream.c",
		"ssl/quic/quic_rx_depack.c",
		"ssl/quic/quic_sf_list.c",
		"ssl/quic/quic_srt_gen.c",
		"ssl/quic/quic_srtm.c",
		"ssl/quic/quic_sstream.c",
		"ssl/quic/quic_statm.c",
		"ssl/quic/quic_stream_map.c",
		"ssl/quic/quic_thread_assist.c",
		"ssl/quic/quic_tls.c",
		"ssl/quic/quic_tls_api.c",
		"ssl/quic/quic_trace.c",
		"ssl/quic/quic_tserver.c",
		"ssl/quic/quic_txp.c",
		"ssl/quic/quic_txpim.c",
		"ssl/quic/quic_types.c",
		"ssl/quic/quic_wire.c",
		"ssl/quic/quic_wire_pkt.c",
		"ssl/quic/uint_set.c",
		"ssl/record/rec_layer_d1.c",
		"ssl/record/rec_layer_s3.c",
		"ssl/record/methods/dtls_meth.c",
		"ssl/record/methods/ssl3_meth.c",
		"ssl/record/methods/tls13_meth.c",
		"ssl/record/methods/tls1_meth.c",
		"ssl/record/methods/tls_common.c",
		"ssl/record/methods/tls_multib.c",
		"ssl/record/methods/tlsany_meth.c",
		"ssl/rio/poll_builder.c",
		"ssl/rio/poll_immediate.c",
		"ssl/rio/rio_notifier.c",
		"ssl/statem/extensions.c",
		"ssl/statem/extensions_clnt.c",
		"ssl/statem/extensions_cust.c",
		"ssl/statem/extensions_srvr.c",
		"ssl/statem/statem.c",
		"ssl/statem/statem_clnt.c",
		"ssl/statem/statem_dtls.c",
		"ssl/statem/statem_lib.c",
		"ssl/statem/statem_srvr.c"
    }

    filter { "action:gmake*", "configurations:Debug"}
        buildoptions { "-ggdb -fPIC -pthread -m64 -Wall" }

    filter { "action:gmake*", "configurations:Release"}
        buildoptions { "-fPIC -pthread -m64 -Wall" }

    filter { "action:gmake*", "configurations:Dist"}
        buildoptions { "-fPIC -pthread -m64 -Wall" }

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"

    filter { "configurations:Dist" }
        defines { "NDEBUG" }
        optimize "Speed"


