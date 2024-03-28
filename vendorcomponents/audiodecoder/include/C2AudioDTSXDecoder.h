#ifndef ANDROID_C2_AUDIO_DTSX_DECODER_H_
#define ANDROID_C2_AUDIO_DTSX_DECODER_H_

#include <C2AudioDecComponent.h>

#define DTSX_LIB_PATH_A "/odm/lib/libHwAudio_dtsx.so"
#define DTSX_LIB64_PATH_A "/odm/lib64/libHwAudio_dtsx.so"

struct DTSXDecoderExternal;
struct AudioInfo;

namespace android {


struct C2AudioDTSXDecoder : public C2AudioDecComponent {

    class IntfImpl;

    C2AudioDTSXDecoder(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2AudioDTSXDecoder();

    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

    //add by amlogic
    struct pcm_info{
        int sample_rate;
        int channel_num;
        int bytes_per_sample;
        int bitstream_type;
    };
    struct pcm_info pcm_out_info;
private:
    enum {
        kNumDelayBlocksMax      = 8,
    };

    std::shared_ptr<IntfImpl> mIntf;

    struct Info {
        uint64_t frameIndex;
        size_t bufferSize;
        uint64_t timestamp;
        std::vector<int32_t> decodedSizes;
    };
    std::list<Info> mBuffersInfo;

    void initPorts();
    status_t initDecoder();
    bool isConfigured() const;
    void drainDecoder();

    void drainOutBuffer(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool,
            bool eos);
    c2_status_t drainEos(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool,
            const std::unique_ptr<C2Work> &work);

    uint32_t maskFromCount(uint32_t channelCount);

    // === Add by amlogic start ===
    bool mInputFlushing;
    bool mOutputFlushing;
    bool mInputFlushDone;
    bool mOutputFlushDone;
    char *mOutputBuffer;
    bool mRunning;
    Mutex mFlushLock;
    DTSXDecoderExternal *mConfig;
    Mutex mConfigLock;
    Mutex mSetUpLock;
    bool mEos;
    bool mSetUp;
    bool mIsFirst;
    bool mAbortPlaying;
    int  nPassThroughEnable;
    int decode_offset;
    bool adec_call;

    void initializeState_l();
    bool setUp();
    bool setUpAudioDecoder_l();
    bool load_dtsx_decoder_lib(const char *filename);
    bool unload_dtsx_decoder_lib();
    bool isSetUp();
    bool tearDown();
    bool tearDownAudioDecoder_l();
    int _aml_dtsx_dualcore_init();
    int _dtsx_pcm_output();
    int _dtsx_raw_output();

    /*dtsx decoder lib function*/
    int (*_aml_dts_decoder_init)(void **ppDtsInstance, unsigned int init_argc, const char *init_argv[]);
    int (*_aml_dts_decoder_process)(void *pDtsInstance, const unsigned char *in_buf, unsigned int in_size, unsigned char **, unsigned int *);
    int (*_aml_dts_decoder_deinit)(void *pDtsInstance);
    int (*_aml_dts_decoder_get_output_info)(void *, int, int *, int *, int *);

    int (*_aml_dts_postprocess_init)(void **ppDtsPPInstance, unsigned int init_argc, const char *init_argv[]);
    int (*_aml_dts_postprocess_deinit)(void *pDtsPPInstance);
    int (*_aml_dts_postprocess_proc)(void *ppDtsPPInstance, const unsigned char *in_buf, unsigned int in_size, unsigned char **, unsigned int *);
    int (*_aml_dts_metadata_update)(void *pDtsInstance, void *pDtsPPInstance);
    int (*_aml_dts_postprocess_get_out_info)(void *, int , int *, int *, int *);
    int (*_aml_dts_postprocess_dynamic_parameter_set)(void *pDtsPPInstance, unsigned int init_argc, const char *init_argv[]);
    void *gDtsxDecoderLibHandler;
    // === Add by amlogic end ===
};

}  // namespace android

#endif  // ANDROID_C2_AUDIO_DTSX_DECODER_H_
