/*
 * The spatial mixer audio unit (kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer, Apple), which
 * 10.10 introduced, renders a multichannel input to an output of a different channel layout. 10.9 has
 * no such component. When the system has none, an in-process component is registered under that
 * description, before any caller can look for it.
 *
 * It takes what a caller sets on a mixer: one input and one output element, each with a linear PCM
 * float, non-interleaved stream format of one sample rate; an audio channel layout on each; an input
 * render callback; and a spatialization algorithm, which it accepts. It renders by pulling the input
 * and mixing each input channel into the output channels by the channels' speaker positions: a speaker
 * the output has receives its channel unchanged; one it lacks is folded into its nearest speakers with
 * the usual downmix coefficients (center and surround channels at -3 dB, a back center channel split
 * between both sides, LFE omitted). Channels a layout does not name map to the output channel of the
 * same index.
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>
#include <string.h>

const char wineskin_audiounit_polyfill = 0;

#define MAX_CHANNELS 32
#define MINUS_3DB 0.70710678f

enum
{
    SPK_FL = 0x1, SPK_FR = 0x2, SPK_FC = 0x4, SPK_LFE = 0x8, SPK_BL = 0x10, SPK_BR = 0x20,
    SPK_FLC = 0x40, SPK_FRC = 0x80, SPK_BC = 0x100, SPK_SL = 0x200, SPK_SR = 0x400, SPK_TC = 0x800,
    SPK_TFL = 0x1000, SPK_TFC = 0x2000, SPK_TFR = 0x4000, SPK_TBL = 0x8000, SPK_TBC = 0x10000,
    SPK_TBR = 0x20000,
};

struct mixer
{
    AudioComponentPlugInInterface iface;
    AudioStreamBasicDescription format[2];   /* input, output */
    UInt32 bitmap[2];
    AURenderCallbackStruct input;
    UInt32 max_frames;
    Boolean initialized;
    float matrix[MAX_CHANNELS][MAX_CHANNELS]; /* [output][input] */
    AudioBufferList *in_list;
    float *in_data, *out_data;
    UInt32 buffer_frames;
};

static int scope_index(AudioUnitScope scope)
{
    return scope == kAudioUnitScope_Input ? 0 : scope == kAudioUnitScope_Output ? 1 : -1;
}

/* The output channel of a speaker, or -1. */
static int out_channel(const struct mixer *m, UInt32 speaker)
{
    UInt32 bitmap = m->bitmap[1], bit;
    int index = 0;

    if (!(bitmap & speaker)) return -1;
    for (bit = 1; bit < speaker; bit <<= 1)
        if (bitmap & bit) index++;
    return index < (int)m->format[1].mChannelsPerFrame ? index : -1;
}

static void add(struct mixer *m, int in, UInt32 speaker, float gain);

static void add_front(struct mixer *m, int in, UInt32 speaker, float gain)
{
    int out = out_channel(m, speaker);

    if (out >= 0)
        m->matrix[out][in] += gain;
    else if (speaker == SPK_FC && (out_channel(m, SPK_FL) >= 0 || out_channel(m, SPK_FR) >= 0))
    {
        add_front(m, in, SPK_FL, gain * MINUS_3DB);
        add_front(m, in, SPK_FR, gain * MINUS_3DB);
    }
    else if (speaker != SPK_FC && out_channel(m, SPK_FC) >= 0)
        m->matrix[out_channel(m, SPK_FC)][in] += gain * MINUS_3DB;
    else if (m->format[1].mChannelsPerFrame == 1)
        m->matrix[0][in] += gain * MINUS_3DB;
}

/* Folds a surround speaker into its other surround speaker of the same side, else the front. */
static void add_surround(struct mixer *m, int in, UInt32 speaker, UInt32 other, UInt32 front, float gain)
{
    int out = out_channel(m, speaker);

    if (out >= 0) m->matrix[out][in] += gain;
    else if ((out = out_channel(m, other)) >= 0) m->matrix[out][in] += gain;
    else add_front(m, in, front, gain * MINUS_3DB);
}

static void add(struct mixer *m, int in, UInt32 speaker, float gain)
{
    int out = out_channel(m, speaker);

    if (out >= 0)
    {
        m->matrix[out][in] += gain;
        return;
    }
    switch (speaker)
    {
    case SPK_FL: case SPK_FR: case SPK_FC: add_front(m, in, speaker, gain); break;
    case SPK_LFE: break;
    case SPK_FLC: add_front(m, in, SPK_FL, gain); break;
    case SPK_FRC: add_front(m, in, SPK_FR, gain); break;
    case SPK_BL: add_surround(m, in, SPK_BL, SPK_SL, SPK_FL, gain); break;
    case SPK_BR: add_surround(m, in, SPK_BR, SPK_SR, SPK_FR, gain); break;
    case SPK_SL: add_surround(m, in, SPK_SL, SPK_BL, SPK_FL, gain); break;
    case SPK_SR: add_surround(m, in, SPK_SR, SPK_BR, SPK_FR, gain); break;
    case SPK_BC:
        if (out_channel(m, SPK_BL) >= 0 || out_channel(m, SPK_SL) >= 0)
        {
            add(m, in, SPK_BL, gain * MINUS_3DB);
            add(m, in, SPK_BR, gain * MINUS_3DB);
        }
        else
        {
            add_front(m, in, SPK_FL, gain * 0.5f);
            add_front(m, in, SPK_FR, gain * 0.5f);
        }
        break;
    case SPK_TC: case SPK_TFC: add(m, in, SPK_FC, gain); break;
    case SPK_TFL: add(m, in, SPK_FL, gain); break;
    case SPK_TFR: add(m, in, SPK_FR, gain); break;
    case SPK_TBL: add(m, in, SPK_BL, gain); break;
    case SPK_TBR: add(m, in, SPK_BR, gain); break;
    case SPK_TBC: add(m, in, SPK_BC, gain); break;
    default:
        if ((UInt32)in < m->format[1].mChannelsPerFrame) m->matrix[in][in] += gain;
        break;
    }
}

static void build_matrix(struct mixer *m)
{
    UInt32 in_channels = m->format[0].mChannelsPerFrame, bitmap = m->bitmap[0], bit = 1;
    UInt32 in;

    memset(m->matrix, 0, sizeof(m->matrix));
    for (in = 0; in < in_channels; in++)
    {
        while (bit && !(bitmap & bit)) bit <<= 1;
        add(m, in, bit, 1.0f);
        if (bit) bit <<= 1;
    }
}

static Boolean format_supported(const AudioStreamBasicDescription *f)
{
    return f->mFormatID == kAudioFormatLinearPCM && (f->mFormatFlags & kAudioFormatFlagIsFloat) &&
           (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) && f->mBitsPerChannel == 32 &&
           f->mChannelsPerFrame >= 1 && f->mChannelsPerFrame <= MAX_CHANNELS;
}

static UInt32 default_bitmap(UInt32 channels)
{
    switch (channels)
    {
    case 1: return SPK_FC;
    case 2: return SPK_FL | SPK_FR;
    case 4: return SPK_FL | SPK_FR | SPK_BL | SPK_BR;
    case 6: return SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_BL | SPK_BR;
    case 8: return SPK_FL | SPK_FR | SPK_FC | SPK_LFE | SPK_BL | SPK_BR | SPK_SL | SPK_SR;
    default: return 0;
    }
}

static OSStatus mixer_initialize(void *self)
{
    struct mixer *m = self;

    if (!format_supported(&m->format[0]) || !format_supported(&m->format[1]) ||
        m->format[0].mSampleRate != m->format[1].mSampleRate)
        return kAudioUnitErr_FormatNotSupported;
    build_matrix(m);
    m->initialized = true;
    return noErr;
}

static OSStatus mixer_uninitialize(void *self)
{
    ((struct mixer *)self)->initialized = false;
    return noErr;
}

static OSStatus mixer_get_property_info(void *self, AudioUnitPropertyID id, AudioUnitScope scope,
                                        AudioUnitElement element, UInt32 *size, Boolean *writable)
{
    UInt32 s;
    Boolean w = true;

    switch (id)
    {
    case kAudioUnitProperty_StreamFormat: s = sizeof(AudioStreamBasicDescription); break;
    case kAudioUnitProperty_AudioChannelLayout: s = sizeof(AudioChannelLayout); break;
    case kAudioUnitProperty_MaximumFramesPerSlice: s = sizeof(UInt32); break;
    case kAudioUnitProperty_SampleRate: s = sizeof(Float64); break;
    case kAudioUnitProperty_SpatializationAlgorithm: s = sizeof(UInt32); break;
    case kAudioUnitProperty_ElementCount: s = sizeof(UInt32); w = false; break;
    case kAudioUnitProperty_Latency: case kAudioUnitProperty_TailTime: s = sizeof(Float64); w = false; break;
    case kAudioUnitProperty_SetRenderCallback: s = sizeof(AURenderCallbackStruct); break;
    default: return kAudioUnitErr_InvalidProperty;
    }
    if (size) *size = s;
    if (writable) *writable = w;
    return noErr;
}

static OSStatus mixer_get_property(void *self, AudioUnitPropertyID id, AudioUnitScope scope,
                                   AudioUnitElement element, void *data, UInt32 *size)
{
    struct mixer *m = self;
    int index = scope_index(scope);

    switch (id)
    {
    case kAudioUnitProperty_StreamFormat:
        if (index < 0 || element) return kAudioUnitErr_InvalidElement;
        if (*size < sizeof(AudioStreamBasicDescription)) return kAudioUnitErr_InvalidPropertyValue;
        memcpy(data, &m->format[index], sizeof(AudioStreamBasicDescription));
        *size = sizeof(AudioStreamBasicDescription);
        return noErr;
    case kAudioUnitProperty_AudioChannelLayout:
    {
        AudioChannelLayout layout = { .mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap };
        if (index < 0 || element) return kAudioUnitErr_InvalidElement;
        if (*size < sizeof(layout)) return kAudioUnitErr_InvalidPropertyValue;
        layout.mChannelBitmap = m->bitmap[index];
        memcpy(data, &layout, sizeof(layout));
        *size = sizeof(layout);
        return noErr;
    }
    case kAudioUnitProperty_MaximumFramesPerSlice:
        *(UInt32 *)data = m->max_frames;
        *size = sizeof(UInt32);
        return noErr;
    case kAudioUnitProperty_SampleRate:
        if (index < 0) return kAudioUnitErr_InvalidScope;
        *(Float64 *)data = m->format[index].mSampleRate;
        *size = sizeof(Float64);
        return noErr;
    case kAudioUnitProperty_SpatializationAlgorithm:
        *(UInt32 *)data = kSpatializationAlgorithm_EqualPowerPanning;
        *size = sizeof(UInt32);
        return noErr;
    case kAudioUnitProperty_ElementCount:
        *(UInt32 *)data = scope == kAudioUnitScope_Global ? 1 : index < 0 ? 0 : 1;
        *size = sizeof(UInt32);
        return noErr;
    case kAudioUnitProperty_Latency:
    case kAudioUnitProperty_TailTime:
        *(Float64 *)data = 0;
        *size = sizeof(Float64);
        return noErr;
    default:
        return kAudioUnitErr_InvalidProperty;
    }
}

static OSStatus mixer_set_property(void *self, AudioUnitPropertyID id, AudioUnitScope scope,
                                   AudioUnitElement element, const void *data, UInt32 size)
{
    struct mixer *m = self;
    int index = scope_index(scope);

    switch (id)
    {
    case kAudioUnitProperty_StreamFormat:
    {
        const AudioStreamBasicDescription *f = data;
        if (index < 0 || element) return kAudioUnitErr_InvalidElement;
        if (size < sizeof(*f)) return kAudioUnitErr_InvalidPropertyValue;
        if (!format_supported(f)) return kAudioUnitErr_FormatNotSupported;
        if (m->initialized) return kAudioUnitErr_Initialized;
        m->format[index] = *f;
        if (!m->bitmap[index] || __builtin_popcount(m->bitmap[index]) != (int)f->mChannelsPerFrame)
            m->bitmap[index] = default_bitmap(f->mChannelsPerFrame);
        return noErr;
    }
    case kAudioUnitProperty_AudioChannelLayout:
    {
        const AudioChannelLayout *layout = data;
        UInt32 bitmap = 0, bitmap_size = sizeof(bitmap);
        if (index < 0 || element) return kAudioUnitErr_InvalidElement;
        if (size < offsetof(AudioChannelLayout, mChannelDescriptions)) return kAudioUnitErr_InvalidPropertyValue;
        if (layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap)
            bitmap = layout->mChannelBitmap;
        else if (AudioFormatGetProperty(kAudioFormatProperty_BitmapForLayoutTag, sizeof(layout->mChannelLayoutTag),
                                        &layout->mChannelLayoutTag, &bitmap_size, &bitmap) != noErr)
            return kAudioUnitErr_InvalidPropertyValue;
        if (m->initialized) return kAudioUnitErr_Initialized;
        m->bitmap[index] = bitmap;
        return noErr;
    }
    case kAudioUnitProperty_MaximumFramesPerSlice:
        if (size < sizeof(UInt32)) return kAudioUnitErr_InvalidPropertyValue;
        m->max_frames = *(const UInt32 *)data;
        return noErr;
    case kAudioUnitProperty_SampleRate:
        if (size < sizeof(Float64) || index < 0) return kAudioUnitErr_InvalidPropertyValue;
        if (m->initialized) return kAudioUnitErr_Initialized;
        m->format[index].mSampleRate = *(const Float64 *)data;
        return noErr;
    case kAudioUnitProperty_SpatializationAlgorithm:
        return noErr;
    case kAudioUnitProperty_SetRenderCallback:
        if (scope != kAudioUnitScope_Input || element || size < sizeof(AURenderCallbackStruct))
            return kAudioUnitErr_InvalidPropertyValue;
        m->input = *(const AURenderCallbackStruct *)data;
        return noErr;
    default:
        return kAudioUnitErr_InvalidProperty;
    }
}

static OSStatus mixer_add_listener(void *self, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc, void *data)
{
    return noErr;
}

static OSStatus mixer_remove_listener(void *self, AudioUnitPropertyID id, AudioUnitPropertyListenerProc proc)
{
    return noErr;
}

static OSStatus mixer_remove_listener_with_data(void *self, AudioUnitPropertyID id,
                                                AudioUnitPropertyListenerProc proc, void *data)
{
    return noErr;
}

static OSStatus mixer_reset(void *self, AudioUnitScope scope, AudioUnitElement element)
{
    return noErr;
}

static Boolean reserve_buffers(struct mixer *m, UInt32 frames)
{
    UInt32 in_channels = m->format[0].mChannelsPerFrame, out_channels = m->format[1].mChannelsPerFrame, i;

    if (m->in_list && frames <= m->buffer_frames) return true;
    free(m->in_list);
    free(m->in_data);
    free(m->out_data);
    m->in_list = malloc(offsetof(AudioBufferList, mBuffers[in_channels]));
    m->in_data = malloc(frames * in_channels * sizeof(float));
    m->out_data = malloc(frames * out_channels * sizeof(float));
    if (!m->in_list || !m->in_data || !m->out_data)
    {
        free(m->in_list);
        free(m->in_data);
        free(m->out_data);
        m->in_list = NULL;
        m->in_data = m->out_data = NULL;
        m->buffer_frames = 0;
        return false;
    }
    m->in_list->mNumberBuffers = in_channels;
    for (i = 0; i < in_channels; i++)
    {
        m->in_list->mBuffers[i].mNumberChannels = 1;
        m->in_list->mBuffers[i].mData = m->in_data + i * frames;
    }
    m->buffer_frames = frames;
    return true;
}

static OSStatus mixer_render(void *self, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
                             UInt32 bus, UInt32 frames, AudioBufferList *data)
{
    struct mixer *m = self;
    UInt32 in_channels = m->format[0].mChannelsPerFrame, out_channels = m->format[1].mChannelsPerFrame;
    AudioUnitRenderActionFlags input_flags = 0;
    UInt32 in, out, i;
    OSStatus status;

    if (!m->initialized) return kAudioUnitErr_Uninitialized;
    if (!m->input.inputProc) return kAudioUnitErr_NoConnection;
    if (bus || data->mNumberBuffers != out_channels) return kAudioUnitErr_InvalidPropertyValue;
    if (frames > m->max_frames) return kAudioUnitErr_TooManyFramesToProcess;
    if (!reserve_buffers(m, frames)) return kAudio_MemFullError;

    for (in = 0; in < in_channels; in++)
        m->in_list->mBuffers[in].mDataByteSize = frames * sizeof(float);
    status = m->input.inputProc(m->input.inputProcRefCon, &input_flags, timestamp, 0, frames, m->in_list);
    if (status != noErr) return status;

    for (out = 0; out < out_channels; out++)
    {
        float *dst = data->mBuffers[out].mData;

        if (!dst) data->mBuffers[out].mData = dst = m->out_data + out * m->buffer_frames;
        data->mBuffers[out].mDataByteSize = frames * sizeof(float);
        memset(dst, 0, frames * sizeof(float));
        for (in = 0; in < in_channels; in++)
        {
            const float *src = m->in_list->mBuffers[in].mData;
            float gain = m->matrix[out][in];
            if (gain == 0.0f) continue;
            for (i = 0; i < frames; i++) dst[i] += gain * src[i];
        }
    }
    if (flags && (input_flags & kAudioUnitRenderAction_OutputIsSilence))
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

static OSStatus mixer_get_parameter(void *self, AudioUnitParameterID id, AudioUnitScope scope,
                                    AudioUnitElement element, AudioUnitParameterValue *value)
{
    return kAudioUnitErr_InvalidParameter;
}

static OSStatus mixer_set_parameter(void *self, AudioUnitParameterID id, AudioUnitScope scope,
                                    AudioUnitElement element, AudioUnitParameterValue value, UInt32 offset)
{
    return kAudioUnitErr_InvalidParameter;
}

static AudioComponentMethod mixer_lookup(SInt16 selector)
{
    switch (selector)
    {
    case kAudioUnitInitializeSelect: return (AudioComponentMethod)mixer_initialize;
    case kAudioUnitUninitializeSelect: return (AudioComponentMethod)mixer_uninitialize;
    case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod)mixer_get_property_info;
    case kAudioUnitGetPropertySelect: return (AudioComponentMethod)mixer_get_property;
    case kAudioUnitSetPropertySelect: return (AudioComponentMethod)mixer_set_property;
    case kAudioUnitAddPropertyListenerSelect: return (AudioComponentMethod)mixer_add_listener;
    case kAudioUnitRemovePropertyListenerSelect: return (AudioComponentMethod)mixer_remove_listener;
    case kAudioUnitRemovePropertyListenerWithUserDataSelect: return (AudioComponentMethod)mixer_remove_listener_with_data;
    case kAudioUnitResetSelect: return (AudioComponentMethod)mixer_reset;
    case kAudioUnitRenderSelect: return (AudioComponentMethod)mixer_render;
    case kAudioUnitGetParameterSelect: return (AudioComponentMethod)mixer_get_parameter;
    case kAudioUnitSetParameterSelect: return (AudioComponentMethod)mixer_set_parameter;
    default: return NULL;
    }
}

static OSStatus mixer_open(void *self, AudioComponentInstance instance)
{
    return noErr;
}

static OSStatus mixer_close(void *self)
{
    struct mixer *m = self;

    free(m->in_list);
    free(m->in_data);
    free(m->out_data);
    free(m);
    return noErr;
}

static AudioComponentPlugInInterface *mixer_factory(const AudioComponentDescription *desc)
{
    struct mixer *m = calloc(1, sizeof(*m));

    if (!m) return NULL;
    m->iface.Open = mixer_open;
    m->iface.Close = mixer_close;
    m->iface.Lookup = mixer_lookup;
    m->max_frames = 4096;
    return &m->iface;
}

__attribute__((constructor))
static void register_spatial_mixer(void)
{
    AudioComponentDescription desc = { kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer,
                                       kAudioUnitManufacturer_Apple, 0, 0 };

    if (!AudioComponentFindNext(NULL, &desc))
        AudioComponentRegister(&desc, CFSTR("Apple: AUSpatialMixer"), 0x10000, mixer_factory);
}
