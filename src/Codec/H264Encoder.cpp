#ifdef ENABLE_X264

#include "H264Encoder.h"
#include "Util/TimeTicker.h"
using namespace toolkit;

namespace mediakit {

H264Encoder::H264Encoder() {}

H264Encoder::~H264Encoder() {
    // * Clear image area
    if (_pPicIn) {
        delete _pPicIn;
        _pPicIn = nullptr;
    }
    if (_pPicOut) {
        delete _pPicOut;
        _pPicOut = nullptr;
    }

    // * Close encoder handle
    if (_pX264Handle) {
        x264_encoder_close(_pX264Handle);
        _pX264Handle = nullptr;
    }
}
 /*typedef struct x264_param_t
 {
 CPU flag bit
 unsigned int cpu;
 int i_threads;  Parallel encoding of multiple frames
 int b_deterministic; Whether to allow non-deterministic thread optimization
 int i_sync_lookahead;  Thread pre-buffering
 
 Video attributes
 int i_width;  Width
 int i_height;  Height
 int i_csp;  CSP of the encoded bitstream, only supports i420, color space setting
 int i_level_idc;  Setting of the level value
 int i_frame_total;  Total number of encoded frames, default 0
 Vui parameter set video availability information video standardization options
 struct
 {
 they will be reduced to be 0 < x <= 65535 and prime
 int i_sar_height;
 int i_sar_width;  Set aspect ratio
 
 int i_overscan;  0=undef, 1=no overscan, 2=overscan Overscan lines, default "undef" (not set), options: show (watch) / crop (remove)
 
 See the following value h264 appendix E
 Int i_vidformat; Video format, default "undef", component/pal/ntsc/secam/mac/undef
 int b_fullrange; Specify full range samples setting, default "off", options: off/on
 int i_colorprim; Original chroma format, default "undef", options: undef/bt709/bt470m/bt470bg, smpte170m/smpte240m/film
 int i_transfer; Conversion method, default "undef", options: undef/bt709/bt470m/bt470bg/linear,log100/log316/smpte170m/smpte240m
 int i_colmatrix; Chroma matrix setting, default "undef", undef/bt709/fcc/bt470bg, smpte170m/smpte240m/GBR/YCgCo
 int i_chroma_loc;  both top & bottom chroma samples specified, range 0~5, default 0
 } vui;
 
 int i_fps_num;
 int i_fps_den;
 These two parameters are determined by the fps frame rate, the assignment process is as follows:
 { float fps;
 if( sscanf( value, "%d/%d", &p->i_fps_num, &p->i_fps_den ) == 2 )
 ;
 else if( sscanf( value, "%f", &fps ) )
 {
 p->i_fps_num = (int)(fps * 1000 + .5);
 p->i_fps_den = 1000;
 }
 else
 b_error = 1;
 }
 The value of Value is fps.
 
 Stream parameters
 int i_frame_reference;  Maximum number of reference frames
 int i_keyint_max;  Set IDR keyframe at this interval
 int i_keyint_min;  Scene switching less than this value encodes as I, not IDR.
 int i_scenecut_threshold; How actively to insert additional I frames
 int i_bframe; Number of P frames between two related images
 int i_bframe_adaptive; Adaptive B frame determination
 int i_bframe_bias; Control the insertion of B frame determination, range -100~+100, the higher the easier it is to insert B frame, default 0
 int b_bframe_pyramid; Allow some B to be reference frames
 Parameters required for deblocking filter
 int b_deblocking_filter;
 int i_deblocking_filter_alphac0;  [-6, 6] -6 light filter, 6 strong
 int i_deblocking_filter_beta;  [-6, 6] idem
 Entropy coding
 int b_cabac;
 int i_cabac_init_idc;
 
 int b_interlaced;  Interlaced scanning
 Quantization
 int i_cqm_preset; Custom quantization matrix (CQM), initialize quantization mode to flat
 char *psz_cqm_file;  JM format reads external quantization matrix file in JM format, automatically ignores other —cqm options
 uint8_t cqm_4iy[16];  used only if i_cqm_preset == X264_CQM_CUSTOM
 uint8_t cqm_4ic[16];
 uint8_t cqm_4py[16];
 uint8_t cqm_4pc[16];
 uint8_t cqm_8iy[64];
 uint8_t cqm_8py[64];
 
 Log
 void (*pf_log)( void *, int i_level, const char *psz, va_list );
 void *p_log_private;
 int i_log_level;
 int b_visualize;
 char *psz_dump_yuv;  Name of the reconstructed frame
 
 Encoding analysis parameters
 struct
 {
 unsigned int intra;  Inter-frame partition
 unsigned int inter;  Intra-frame partition
 
 int b_transform_8x8;  Inter-frame partition
 int b_weighted_bipred; Implicit weighting for b frames
 int i_direct_mv_pred; Time-space motion prediction
 int i_chroma_qp_offset; Chroma quantization step offset
 
 int i_me_method;  Motion estimation algorithm (X264_ME_*)
 int i_me_range;  Integer pixel motion estimation search range (from predicted mv)
 int i_mv_range;  Maximum length of motion vector (in pixels). -1 = auto, based on level
 int i_mv_range_thread;  Minimum space between threads. -1 = auto, based on number of threads.
 int i_subpel_refine;  Sub-pixel motion estimation quality
 int b_chroma_me;  Sub-pixel chroma motion estimation and mode selection for P frames
 int b_mixed_references; Allow each macroblock partition in the P frame to have its own reference number
 int i_trellis;  Trellis quantization, find the appropriate quantization value for each 8x8 block, requires CABAC, default 0 0: off 1: use only at the end of encoding 2: always use
 int b_fast_pskip; Fast P frame skip detection
 int b_dct_decimate;  Transform parameter domain in P-frames
 int i_noise_reduction; Adaptive pseudo-blind area
 float f_psy_rd;  Psy RD strength
 float f_psy_trellis;  Psy trellis strength
 int b_psy;  Toggle all psy optimizations
 
 , the size of the invalid area used in luminance quantization
 int i_luma_deadzone[2];  {Inter-frame, Intra-frame}
 
 int b_psnr;  Calculate and print PSNR information
 int b_ssim; Calculate and print SSIM information
 } analyse;
 
 Bitrate control parameters
 struct
 {
 int i_rc_method;  X264_RC_*
 
 int i_qp_constant;  0-51
 int i_qp_min; Minimum quantization value allowed
 int i_qp_max; Maximum quantization value allowed
 int i_qp_step; Maximum quantization step between frames
 
 int i_bitrate; Set average bitrate
 float f_rf_constant;  1pass VBR, nominal QP
 float f_rate_tolerance;
 int i_vbv_max_bitrate; In average bitrate mode, the maximum instantaneous bitrate, default 0 (same as -B setting)
 int i_vbv_buffer_size; Size of the bitrate control buffer, unit kbit, default 0
 float f_vbv_buffer_init;  <=1: fraction of buffer_size. >1: kbit bitrate control buffer data retention maximum data amount ratio to buffer size, range 0~1.0, default 0.9
 float f_ip_factor;
 float f_pb_factor;
 
 int i_aq_mode;  psy adaptive QP. (X264_AQ_*)
 float f_aq_strength;
 int b_mb_tree;  Macroblock-tree ratecontrol.
 int i_lookahead;
 
 2pass multiple compression bitrate control
 int b_stat_write;  Enable stat writing in psz_stat_out
 char *psz_stat_out;
 int b_stat_read;  Read stat from psz_stat_in and use it
 char *psz_stat_in;
 
 2pass params (same as ffmpeg ones)
 float f_qcompress;  0.0 => cbr, 1.0 => constant qp
 float f_qblur; Quantization blur over time
 float f_complexity_blur;  Complexity blur over time
 x264_zone_t *zones;  Bitrate control coverage
 int i_zones;  number of zone_t's
 char *psz_zones; Another way to specify the zone
 } rc;
 
 Muxing parameters
 int b_aud; Generate access unit delimiter
 int b_repeat_headers;  Place SPS/PPS before each keyframe
 int i_sps_id;  SPS and PPS id number
 
 Slice (like strip) parameters
 int i_slice_max_size;  Maximum number of bytes per slice, including expected NAL overhead.
 int i_slice_max_mbs;  Maximum number of macroblocks per slice, overwrite i_slice_count
 int i_slice_count;  Number of strips per frame: Set rectangular strips.
 
 Optional callback for freeing this x264_param_t when it is done being used.
 * Only used when the x264_param_t sits in memory for an indefinite period of time,
 * i.e. when an x264_param_t is passed to x264_t in an x264_picture_t or in zones.
 * Not used when x264_encoder_reconfig is called directly.
 void (*param_free)( void* );
} x264_param_t;*/

bool H264Encoder::init(int iWidth, int iHeight, int iFps, int iBitRate) {
    if (_pX264Handle) {
        return true;
    }
    x264_param_t X264Param, *pX264Param = &X264Param;
    // * Configure parameters
    // * Use default parameters
    x264_param_default_preset(pX264Param, "ultrafast", "zerolatency");

    //* cpuFlags
    pX264Param->i_threads = X264_SYNC_LOOKAHEAD_AUTO;        //* The guarantee that the buffer will continue to use the undeadlocked buffer.
    //* video Properties
    pX264Param->i_width = iWidth; //* width.
    pX264Param->i_height = iHeight; //* high
    pX264Param->i_frame_total = 0; //* Total number of encoded frames. Don't know if you use 0.
    pX264Param->i_keyint_max = iFps * 3; //ffmpeg:gop_size Maximum keyframe interval
    pX264Param->i_keyint_min = iFps * 1; //ffmpeg:keyint_min Minimum keyframe interval
    //* Rate control Parameters
    pX264Param->rc.i_bitrate = iBitRate / 1000;        //* Bit rate (bit rate, unit Kbps)
    pX264Param->rc.i_qp_step = 1; // ffmpeg:max_qdiff The maximum amount of change in quantization factor that performs shear between frames.
    pX264Param->rc.i_qp_min = 10;    //ffmpeg:qmin The smallest quantization factor. Value range 1-51. It is recommended to be between 10-30。
    pX264Param->rc.i_qp_max = 41;    //ffmpeg:qmax The largest quantization factor. Value range 1-51. It is recommended to be between 10-30.
    pX264Param->rc.f_qcompress = 0.6; // ffmpeg:qcompress The compression ratio of the quantizer is 0-1. The smaller the bit rate, the more the region is
                                      // fixed, but the higher the quantizer parameters are, the more fixed.
    pX264Param->analyse.i_me_range = 16;        //ffmpeg:me_range Radius of motion detection
    pX264Param->i_frame_reference = 3;        //ffmpeg:refsB and P frames forward predict the reference frame number. Value range 1-16.
    // This value does not affect the decoding speed, but the larger the decoding
    // The more memory required. This value is generally
    // The better the effect, but the effect is not obvious after 6
    // It's not obvious.

    pX264Param->analyse.i_trellis = 1;                            //ffmpeg:trellis
    // pX264Param->analyse.i_me_method=X264_ME_DIA;//ffmpeg:me_method ME_ZERO Motion detection method
    pX264Param->rc.f_qblur = 0.5;        //ffmpeg:qblur

    //* bitstream parameters
     /*open-GOP
     open-GOP only appears when the bitstream contains B frames.
     A frame in a GOP needs to rely on some frames in the previous GOP when decoding,
     This GOP is called open-GOP.
     Some decoders do not fully support open-GOP bitstreams,
     For example, Blu-ray decoders, so open-GOP is disabled by default in x264.
     For the decoding end, if the received bitstream is as follows: I0 B0 B1 P0 B2 B3... This is an open-GOP bitstream (I frame followed by B frame).
     Therefore, the decoding of B0 B1 needs to use the data of the GOP before I0, and the dts of B0 B1 is less than that of I0.
     If the bitstream is as follows: I0 P0 B0 B1 P1 B2 B3... This is a close-GOP bitstream,
     The decoding of all frames after I0 does not depend on the frames before I0, and the dts of all frames after I0 is greater than that of I0.
     If the bitstream is IDR0 B0 B1 P0 B2 B3... then this GOP is close-GOP, although the dst of B0, B1 is smaller than that of IDR0,
     But both the encoder and decoder refresh the reference buffer, B0, B1 cannot refer to the forward GOP frame.
     For the encoding end, if the encoding frame type is determined as follows: ...P0 B1 B2 P3 B4 B5 I6 This will output an open-Gop bitstream (P0 P3 B1 B2 I6 B4 B5...),
     The decoding of B4 B5 depends on P3.
     If the encoding frame type is determined as follows...P0 B1 B2 P3 B4 P5 I6, then this will not output an open-GOP bitstream (P0 P3 B1 B2 P5 B4 I6...).
     The difference between the two is whether the 5th frame before I6 is set to B frame or P frame,
     If the last frame of a GOP (the 5th frame in the example above) is set to B frame,
     This bitstream is open-GOP, and setting it to P frame is close-GOP.
     Since B frames have better compression performance than P frames, open-GOP performs slightly better than close-GOP in terms of encoding performance,
     But for compatibility and less trouble, it's better to turn off opne-GOP.
    */
    pX264Param->b_open_gop = 0;
    pX264Param->i_bframe = 0;        //Maximum number of B frames.
    pX264Param->i_bframe_pyramid = 0;
    pX264Param->i_bframe_adaptive = X264_B_ADAPT_TRELLIS;
    //* Log
    pX264Param->i_log_level = X264_LOG_ERROR;

    //* muxing parameters
    pX264Param->i_fps_den = 1; //* Frame rate denominator
    pX264Param->i_fps_num = iFps; //* Frame rate molecule
    pX264Param->i_timebase_den = pX264Param->i_fps_num;
    pX264Param->i_timebase_num = pX264Param->i_fps_den;

    pX264Param->analyse.i_subpel_refine = 1; //This parameter controls the trade-off between mass and velocity during motion estimation. Subq=5 can be compressed >10% at subq=1.1-7
    pX264Param->analyse.b_fast_pskip = 1; //Perform early fast jump detection within P frames. This often increases the speed without any loss.

    pX264Param->b_annexb = 1; //1 The front is 0x00000001, and 0 is the nal length
    pX264Param->b_repeat_headers = 1; //Whether to put the sps and pps frames in front of the keyframe, 0 No 1,

    // * Set Profile. Use baseline
    x264_param_apply_profile(pX264Param, "high");

    // * Open encoder handle, get the settings for X264 through x264_encoder_parameters
    // * Parameters. Update the parameters of X264 through x264_encoder_reconfig
    _pX264Handle = x264_encoder_open(pX264Param);
    if (!_pX264Handle) {
        return false;
    }
    _pPicIn = new x264_picture_t;
    _pPicOut = new x264_picture_t;
    x264_picture_init(_pPicIn);
    x264_picture_init(_pPicOut);
    _pPicIn->img.i_csp = X264_CSP_I420;
    _pPicIn->img.i_plane = 3;
    return true;
}

int H264Encoder::inputData(char *yuv[3], int linesize[3], int64_t cts, H264Frame **out_frame) {
    //TimeTicker1(5);
    _pPicIn->img.i_stride[0] = linesize[0];
    _pPicIn->img.i_stride[1] = linesize[1];
    _pPicIn->img.i_stride[2] = linesize[2];
    _pPicIn->img.plane[0] = (uint8_t *) yuv[0];
    _pPicIn->img.plane[1] = (uint8_t *) yuv[1];
    _pPicIn->img.plane[2] = (uint8_t *) yuv[2];
    _pPicIn->i_pts = cts;
    int iNal;
    x264_nal_t *pNals;

    int iResult = x264_encoder_encode(_pX264Handle, &pNals, &iNal, _pPicIn, _pPicOut);
    if (iResult <= 0) {
        return 0;
    }
    for (int i = 0; i < iNal; i++) {
        x264_nal_t pNal = pNals[i];
        _aFrames[i].iType = pNal.i_type;
        _aFrames[i].iLength = pNal.i_payload;
        _aFrames[i].pucData = pNal.p_payload;
        _aFrames[i].dts = _pPicOut->i_dts;
        _aFrames[i].pts = _pPicOut->i_pts;
    }
    *out_frame = _aFrames;
    return iNal;
}

} /* namespace mediakit */

#endif //ENABLE_X264
















