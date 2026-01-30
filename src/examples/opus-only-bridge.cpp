#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>

#include "../gstutil/auto-gst-object.hpp"
#include "../gstutil/pipeline-helper.hpp"
#include "../macros/autoptr.hpp"
#include "../macros/unwrap.hpp"
#include "../util/argument-parser.hpp"
#include "glib-object.h"
#include "helper.hpp"

namespace {
declare_autoptr(GstCaps, GstCaps, gst_caps_unref);
declare_autoptr(GMainLoop, GMainLoop, g_main_loop_unref);
declare_autoptr(GstMessage, GstMessage, gst_message_unref);
declare_autoptr(GString, gchar, g_free);

// callbacks
struct Context {
    GstElement* pipeline;
    const char* client_ip;
    int         client_port;
};

auto jitsibin_pad_added_handler(GstElement* const /*jitsibin*/, GstPad* const pad, gpointer const data) -> void {
    auto& self = *std::bit_cast<Context*>(data);

    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    PRINT("pad added name={}", name);

    unwrap(pad_name, parse_jitsibin_pad_name(name));

    auto decoder = std::string();
    if(pad_name.codec == "OPUS") {
        unwrap_mut(rtppay, add_new_element_to_pipeine(self.pipeline, "rtpopuspay"));
        unwrap_mut(udpsink, add_new_element_to_pipeine(self.pipeline, "udpsink"));

        g_object_set(&udpsink,
                     "host", self.client_ip,
                     "port", self.client_port,
                     "async", FALSE,
                     NULL);

        const auto rtppay_sink_pad = AutoGstObject(gst_element_get_static_pad(&rtppay, "sink"));
        ensure(gst_pad_link(pad, rtppay_sink_pad.get()) == GST_PAD_LINK_OK);
        ensure(gst_element_link(&rtppay, &udpsink) == TRUE);

        ensure(gst_element_sync_state_with_parent(&rtppay) == TRUE);
        ensure(gst_element_sync_state_with_parent(&udpsink) == TRUE);

        PRINT("added opus audio payloader");
        return;
    } else {
        PRINT("unsupported codec {}", pad_name.codec);
        decoder = "fakesink";
        unwrap_mut(fakesink, add_new_element_to_pipeine(self.pipeline, "fakesink"));
        const auto fakesink_sink_pad = AutoGstObject(gst_element_get_static_pad(&fakesink, "sink"));
        ensure(fakesink_sink_pad.get() != NULL);
        ensure(gst_pad_link(pad, fakesink_sink_pad.get()) == GST_PAD_LINK_OK);
        ensure(gst_element_sync_state_with_parent(&fakesink) == TRUE);
        return;
    }
}

auto jitsibin_pad_removed_handler(GstElement* const /*jitisbin*/, GstPad* const pad, gpointer const /*data*/) -> void {
    const auto name_g = AutoGString(gst_object_get_name(GST_OBJECT(pad)));
    const auto name   = std::string_view(name_g.get());
    PRINT("pad removed name={}", name);
}

auto jitsibin_participant_joined_handler(GstElement* const /*jitisbin*/, const gchar* const participant_id, const gchar* const nick, gpointer const /*data*/) -> void {
    PRINT("participant joined id={} nick={}", participant_id, nick);
}

auto jitsibin_participant_left_handler(GstElement* const /*jitisbin*/, const gchar* const participant_id, const gchar* const nick, gpointer const /*data*/) -> void {
    PRINT("participant left id={} nick={}", participant_id, nick);
}

auto jitsibin_mute_state_changed_handler(GstElement* const /*jitisbin*/, const gchar* const participant_id, const gboolean is_audio, const gboolean new_muted, gpointer const /*data*/) -> void {
    PRINT("mute state changed id={} {}={}", participant_id, is_audio ? "audio" : "video", new_muted);
}

auto jitsibin_finished_handler(GstElement* const /*jitisbin*/, const gboolean success, gpointer const data) -> void {
    PRINT("finished success={}", success);
    const auto& self = *std::bit_cast<Context*>(data);
    const auto  bus  = AutoGstObject(gst_element_get_bus(self.pipeline));
    ensure(gst_bus_post(bus.get(), gst_message_new_eos(NULL)) == TRUE);
}
} // namespace

auto main(const int argc, const char* const* argv) -> int {
    const char* host      = nullptr;
    const char* room      = nullptr;
    const char* client_ip = nullptr;
    int         client_port;
    {
        auto help   = false;
        auto parser = args::Parser<>();
        parser.arg(&host, "HOST", "server domain");
        parser.arg(&room, "ROOM", "room name");
        parser.arg(&client_ip, "CLIENT_IP", "IPv4 address of the client");
        parser.arg(&client_port, "CLIENT_PORT", "UDP port number of the client");
        parser.kwflag(&help, {"-h", "--help"}, "print this help message", {.no_error_check = true});
        if(!parser.parse(argc, argv) || help) {
            std::println("usage: example {}", parser.get_help());
            return 0;
        }
    }
    const int recv_port = client_port;

    gst_init(NULL, NULL);

    const auto pipeline = AutoGstObject(gst_pipeline_new(NULL));
    ensure(pipeline.get() != NULL);

    auto context = Context{
        .pipeline    = pipeline.get(),
        .client_ip   = client_ip,
        .client_port = client_port,
    };

    /*
     * udpsrc -> jtpjitterbuffer -> rtpopusdepay -> jitsibin
     */
    unwrap_mut(udpsrc, add_new_element_to_pipeine(pipeline.get(), "udpsrc"));
    g_object_set(&udpsrc,
                 "port", recv_port,
                 NULL);
    const auto caps = AutoGstCaps(gst_caps_from_string("application/x-rtp,media=audio,encoding-name=OPUS,payload=96"));
    g_object_set(&udpsrc, "caps", caps.get(), NULL);
    unwrap_mut(rtpjitterbuffer, add_new_element_to_pipeine(pipeline.get(), "rtpjitterbuffer"));
    g_object_set(&rtpjitterbuffer,
                 "latency", 150,
                 "drop-on-latency", TRUE,
                 NULL);
    unwrap_mut(rtpopusdepay, add_new_element_to_pipeine(pipeline.get(), "rtpopusdepay"));

    unwrap_mut(jitsibin, add_new_element_to_pipeine(pipeline.get(), "jitsibin"));
    g_signal_connect(&jitsibin, "pad-added", G_CALLBACK(jitsibin_pad_added_handler), &context);
    g_signal_connect(&jitsibin, "pad-removed", G_CALLBACK(jitsibin_pad_removed_handler), &context);
    g_signal_connect(&jitsibin, "participant-joined", G_CALLBACK(jitsibin_participant_joined_handler), &context);
    g_signal_connect(&jitsibin, "participant-left", G_CALLBACK(jitsibin_participant_left_handler), &context);
    g_signal_connect(&jitsibin, "mute-state-changed", G_CALLBACK(jitsibin_mute_state_changed_handler), &context);
    g_signal_connect(&jitsibin, "finished", G_CALLBACK(jitsibin_finished_handler), &context);

    g_object_set(&jitsibin,
                 "server", host,
                 "room", room,
                 "nick", "gstjitsimeet-example",
                 "receive-limit", 3,
                 "force-play", TRUE,
                 "insecure", TRUE,
                 NULL);

    ensure(gst_element_link_pads(&udpsrc, NULL, &rtpjitterbuffer, NULL) == TRUE);
    ensure(gst_element_link_pads(&rtpjitterbuffer, NULL, &rtpopusdepay, NULL) == TRUE);
    ensure(gst_element_link_pads(&rtpopusdepay, NULL, &jitsibin, "audio_sink") == TRUE);

    ensure(run_pipeline(pipeline.get()));

    return 0;
}
