#pragma once

#define PIPE_VIDEO_VP8 "rtpbin name=sess_vid rtp-profile=3 \
	udpsrc port=%d caps=\"application/x-rtp, media=video, payload=%d, encoding-name=VP8, clock-rate=90000, rtcp-fb-nack-pli=1, rtp-profile=3\" name=udp_rtp_src_video  \
	! sess_vid.recv_rtp_sink_0 \
	sess_vid. ! rtpvp8depay name=depay_vid \
	udpsrc port=%d ! sess_vid.recv_rtcp_sink_0 \
	sess_vid.send_rtcp_src_0 ! udpsink port=%d sync=false async=false \
	depay_vid. ! rtpvp8pay pt=96"

#define PIPE_VIDEO_VP9 "rtpbin name=sess_vid rtp-profile=3 \
	udpsrc port=%d caps=\"application/x-rtp, media=video, payload=%d, encoding-name=VP9, clock-rate=90000, rtcp-fb-nack-pli=1, rtp-profile=3\" name=udp_rtp_src_video  \
	! sess_vid.recv_rtp_sink_0 \
	sess_vid. ! rtpvp9depay name=depay_vid \
	udpsrc port=%d ! sess_vid.recv_rtcp_sink_0 \
	sess_vid.send_rtcp_src_0 ! udpsink port=%d sync=false async=false \
	depay_vid. ! rtpvp9pay pt=96"

#define PIPE_VIDEO_H264 "rtpbin name=sess_vid rtp-profile=3 \
	udpsrc port=%d caps=\"application/x-rtp, media=video, payload=%d, encoding-name=H264, clock-rate=90000, rtcp-fb-nack-pli=1, rtp-profile=3\" name=udp_rtp_src_video  \
	! sess_vid.recv_rtp_sink_0 \
	sess_vid. ! rtph264depay name=depay_vid \
	udpsrc port=%d ! sess_vid.recv_rtcp_sink_0 \
	sess_vid.send_rtcp_src_0 ! udpsink port=%d sync=false async=false \
	depay_vid. ! rtph264pay pt=96"
	
#define PIPE_AUDIO_OPUS "udpsrc port=%d name=udp_rtp_src_audio ! application/x-rtp, media=audio, payload=%d, encoding-name=OPUS, clock-rate=48000, rtp-profile=3 ! .recv_rtp_sink rtpsession name=sess_aud \
	.recv_rtp_src ! rtpopusdepay name=depay_aud \
	udpsrc port=%d ! sess_aud.recv_rtcp_sink \
	sess_aud.send_rtcp_src ! udpsink port=%d \
	depay_aud. ! audio/x-opus, channels=1 ! rtpopuspay pt=127"
