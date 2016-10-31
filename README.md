# idilia-source-plugin

Source plugin for <b>Janus WebRTC Gateway v0.2.0</b> based on orginal echotest plugin. This source plugin incorporates GStreamer Multimedia Framework at the backend and implements RTSP server.

## Building instructions

Follow the instructions from https://github.com/meetecho/janus-gateway/blob/master/README.md and then:

    git clone https://github.com/MotorolaSolutions/idilia-source-plugin.git
    cd idilia-source-plugin
    sh autogen.sh
    ./configure
    sudo make install configs
