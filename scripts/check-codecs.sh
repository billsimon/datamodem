#!/bin/sh
# Check that a release binary, unstripped, carries G.711 and none of the
# other codecs - or SRTP, or Speex's echo canceller - that pjproject can
# bundle. build-deps.sh leaves them out: datamodem never uses them, and
# they come with licences of their own.
#
#   scripts/check-codecs.sh build/datamodem
set -eu

if ! nm "$1" | grep -q 'pjmedia_codec_g711_init'; then
    echo "::error::$1 has no G.711 codec, or no symbols to check"
    exit 1
fi
if nm "$1" | grep -E 'pjmedia_codec_(l16|gsm|g722|g7221|speex|ilbc)_init|pjmedia_transport_srtp_create|speex_echo_state_init'; then
    echo "::error::$1 carries a codec it should not"
    exit 1
fi
echo "$1: G.711 only"
