#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <opus.h>

/* libopus' BSD license is packaged by the authenticated Android build script. */
typedef struct { OpusEncoder *encoder; OpusDecoder *decoder; int lookahead; } Voice;
#define JNI_FN(name) Java_br_com_ideiarec_companion_voice_OpusCodec_##name
#define VOICE(value) ((Voice *)(intptr_t)(value))

JNIEXPORT jlong JNICALL JNI_FN(create)(JNIEnv *env, jclass cls, jint bitrate) {
    (void)env; (void)cls;
    if (bitrate != 64000 && bitrate != 48000 && bitrate != 32000) return 0;
    Voice *v = calloc(1, sizeof(*v));
    if (!v) return 0;
    int error;
    v->encoder = opus_encoder_create(24000, 1, OPUS_APPLICATION_AUDIO, &error);
    if (!v->encoder || error != OPUS_OK) goto fail;
    v->decoder = opus_decoder_create(16000, 1, &error);
    if (!v->decoder || error != OPUS_OK) goto fail;
    if (opus_encoder_ctl(v->encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_SET_VBR_CONSTRAINT(1)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
        opus_encoder_ctl(v->encoder, OPUS_GET_LOOKAHEAD(&v->lookahead)) != OPUS_OK) goto fail;
    return (jlong)(intptr_t)v;
fail:
    if (v->encoder) opus_encoder_destroy(v->encoder);
    if (v->decoder) opus_decoder_destroy(v->decoder);
    free(v); return 0;
}
JNIEXPORT jint JNICALL JNI_FN(lookahead)(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls; return VOICE(handle)->lookahead;
}
JNIEXPORT jbyteArray JNICALL JNI_FN(encode)(JNIEnv *env, jclass cls, jlong handle, jshortArray pcm) {
    (void)cls;
    if ((*env)->GetArrayLength(env, pcm) != 480) return NULL;
    opus_int16 samples[480]; unsigned char packet[1276];
    (*env)->GetShortArrayRegion(env, pcm, 0, 480, samples);
    int n = opus_encode(VOICE(handle)->encoder, samples, 480, packet, sizeof(packet));
    if (n <= 0) return NULL;
    jbyteArray result = (*env)->NewByteArray(env, n);
    if (result) (*env)->SetByteArrayRegion(env, result, 0, n, (jbyte *)packet);
    return result;
}
JNIEXPORT jshortArray JNICALL JNI_FN(decode)(JNIEnv *env, jclass cls, jlong handle, jbyteArray data) {
    (void)cls;
    int n = (*env)->GetArrayLength(env, data);
    if (n <= 0 || n > 1276) return NULL;
    unsigned char packet[1276]; opus_int16 samples[320];
    (*env)->GetByteArrayRegion(env, data, 0, n, (jbyte *)packet);
    if (opus_packet_get_nb_samples(packet, n, 16000) != 320) return NULL;
    n = opus_decode(VOICE(handle)->decoder, packet, n, samples, 320, 0);
    if (n != 320) return NULL;
    jshortArray result = (*env)->NewShortArray(env, 320);
    if (result) (*env)->SetShortArrayRegion(env, result, 0, 320, samples);
    return result;
}
JNIEXPORT jboolean JNICALL JNI_FN(reset)(JNIEnv *env, jclass cls, jlong handle, jboolean encoder) {
    (void)env; (void)cls;
    Voice *v = VOICE(handle);
    return (encoder ? opus_encoder_ctl(v->encoder, OPUS_RESET_STATE) : opus_decoder_ctl(v->decoder, OPUS_RESET_STATE)) == OPUS_OK;
}
JNIEXPORT void JNICALL JNI_FN(destroy)(JNIEnv *env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    Voice *v = VOICE(handle);
    opus_encoder_destroy(v->encoder); opus_decoder_destroy(v->decoder); free(v);
}
