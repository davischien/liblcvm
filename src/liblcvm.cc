// liblcvm: Low-Complexity Video Metrics Library.
// A library to detect frame dups and video freezes.

// A show case of using [ISOBMFF](https://github.com/DigiDNA/ISOBMFF) to
// detect frame dups and video freezes in ISOBMFF files.

#include "liblcvm.h"

#include <h264_bitstream_parser.h>
#include <h264_common.h>
#include <h264_nal_unit_parser.h>
#include <h265_bitstream_parser.h>
#include <h265_common.h>
#include <h265_nal_unit_parser.h>
#include <stdint.h>  // for uint32_t, uint64_t
#include <sys/stat.h>

#include <ISOBMFF.hpp>         // for various
#include <ISOBMFF/Parser.hpp>  // for Parser
#include <algorithm>           // for sort
#include <cmath>               // for sqrt
#include <cstdio>              // for fprintf, stderr, stdout
#include <memory>              // for shared_ptr, operator==, __shared...
#include <numeric>             // for accumulate
#include <string>              // for basic_string, string
#include <vector>              // for vector

#include "config.h"

#define MAX_AUDIO_VIDEO_RATIO 1.05

int get_liblcvm_version(std::string &version) {
  version = PROJECT_VER;
  return 0;
}

int get_timing_information(std::shared_ptr<ISOBMFF::ContainerBox> stbl,
                           uint32_t timescale_hz,
                           struct IsobmffFileInformation &info, int debug);

int get_keyframe_information(std::shared_ptr<ISOBMFF::ContainerBox> stbl,
                             struct IsobmffFileInformation &info, int debug);

int get_frame_information(std::shared_ptr<ISOBMFF::ContainerBox> stbl,
                          struct IsobmffFileInformation &info, int debug);

int derive_timing_info(struct TimingInformation &timing, bool sort_by_pts,
                       int debug);

int get_isobmff_information(const char *infile,
                            struct IsobmffFileInformation &info,
                            bool sort_by_pts, int debug) {
  // 0. store the filename
  info.filename = infile;

  // 1. parse the input file
  ISOBMFF::Parser parser;
  try {
    parser.Parse(info.filename.c_str());
  } catch (std::runtime_error &e) {
    return -1;
  }
  std::shared_ptr<ISOBMFF::File> file = parser.GetFile();
  if (file == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no file in %s\n", info.filename.c_str());
    }
    return -1;
  }

  // 2. look for a moov container box
  std::shared_ptr<ISOBMFF::ContainerBox> moov =
      file->GetTypedBox<ISOBMFF::ContainerBox>("moov");
  if (moov == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no /moov in %s\n", info.filename.c_str());
    }
    return -1;
  }

  // 3. look for trak container boxes
  info.timing.duration_video_sec = -1.0;
  info.timing.duration_audio_sec = -1.0;
  for (auto &box : moov->GetBoxes()) {
    std::string name = box->GetName();
    if (name.compare("trak") != 0) {
      continue;
    }
    auto trak = std::dynamic_pointer_cast<ISOBMFF::ContainerBox>(box);

    // 4. look for a mdia container box
    std::shared_ptr<ISOBMFF::ContainerBox> mdia =
        trak->GetTypedBox<ISOBMFF::ContainerBox>("mdia");
    if (mdia == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia in %s\n",
                info.filename.c_str());
      }
      return -1;
    }

    // 5. look for a hdlr box
    std::shared_ptr<ISOBMFF::HDLR> hdlr =
        mdia->GetTypedBox<ISOBMFF::HDLR>("hdlr");
    if (hdlr == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/hdlr in %s\n",
                info.filename.c_str());
      }
      return -1;
    }
    std::string handler_type = hdlr->GetHandlerType();

    // 6. look for a mdhd box
    std::shared_ptr<ISOBMFF::MDHD> mdhd =
        mdia->GetTypedBox<ISOBMFF::MDHD>("mdhd");
    if (mdhd == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/mdhd in %s\n",
                info.filename.c_str());
      }
      return -1;
    }
    uint32_t timescale_hz = mdhd->GetTimescale();
    uint64_t duration = mdhd->GetDuration();
    float duration_sec = (float)duration / timescale_hz;
    if (debug > 1) {
      fprintf(stdout, "-> handler_type: %s ", handler_type.c_str());
      fprintf(stdout, "timescale: %u ", timescale_hz);
      fprintf(stdout, "duration: %lu ", duration);
      fprintf(stdout, "duration_sec: %f\n", duration_sec);
    }
    if (handler_type.compare("soun") == 0) {
      info.timing.duration_audio_sec = duration_sec;
      info.timing.timescale_audio_hz = timescale_hz;
    } else if (handler_type.compare("vide") == 0) {
      info.timing.duration_video_sec = duration_sec;
      info.timing.timescale_video_hz = timescale_hz;
    }

    // only keep video processing
    if (handler_type.compare("vide") != 0) {
      continue;
    }

    // 7. look for a tkhd box
    std::shared_ptr<ISOBMFF::TKHD> tkhd =
        trak->GetTypedBox<ISOBMFF::TKHD>("tkhd");
    if (tkhd == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/tkhd in %s\n",
                info.filename.c_str());
      }
      return -1;
    }
    info.frame.width = tkhd->GetWidth();
    info.frame.height = tkhd->GetHeight();

    // 8. look for a minf container box
    std::shared_ptr<ISOBMFF::ContainerBox> minf =
        mdia->GetTypedBox<ISOBMFF::ContainerBox>("minf");
    if (minf == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf in %s\n",
                info.filename.c_str());
      }
      return -1;
    }

    // 9. look for a stbl container box
    std::shared_ptr<ISOBMFF::ContainerBox> stbl =
        minf->GetTypedBox<ISOBMFF::ContainerBox>("stbl");
    if (stbl == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl in %s\n",
                info.filename.c_str());
      }
      return -1;
    }

    // 10. get video timing information
    // init timing info
    info.timing.num_video_frames = 0;
    info.timing.dts_sec_list.clear();
    info.timing.pts_sec_list.clear();
    info.timing.stts_unit_list.clear();
    info.timing.ctts_unit_list.clear();
    // first frame starts at 0.0
    info.timing.dts_sec_list.push_back(0.0);
    info.timing.pts_sec_list.push_back(0.0);
    if (get_timing_information(stbl, timescale_hz, info, debug) < 0) {
      if (debug > 0) {
        fprintf(stderr, "error: no timing information in %s\n",
                info.filename.c_str());
      }
      return -1;
    }

    // 11. get video keyframe information
    info.timing.keyframe_sample_number_list.clear();
    if (get_keyframe_information(stbl, info, debug) < 0) {
      if (debug > 0) {
        fprintf(stderr, "error: no keyframe information in %s\n",
                info.filename.c_str());
      }
      return -1;
    }

    // 12. get video frame information
    if (get_frame_information(stbl, info, debug) < 0) {
      if (debug > 0) {
        fprintf(stderr, "error: no frame information in %s\n",
                info.filename.c_str());
      }
      return -1;
    }
  }

  // 13. derive timing info
  if (derive_timing_info(info.timing, sort_by_pts, debug) < 0) {
    if (debug > 0) {
      fprintf(stderr, "error: cannot derive timing information in %s\n",
              info.filename.c_str());
    }
    return -1;
  }

  return 0;
}

int get_timing_information(std::shared_ptr<ISOBMFF::ContainerBox> stbl,
                           uint32_t timescale_hz,
                           struct IsobmffFileInformation &info, int debug) {
  // 1. look for a stts box
  std::shared_ptr<ISOBMFF::STTS> stts =
      stbl->GetTypedBox<ISOBMFF::STTS>("stts");
  if (stts == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl/stts in %s\n",
              info.filename.c_str());
    }
    return -1;
  }

  // 2. gather the stts timestamp durations
  // run all through the stts table
  uint32_t stts_sample_count = 0;
  uint32_t last_dts_unit = 0.0;
  for (unsigned int i = 0; i < stts->GetEntryCount(); i++) {
    uint32_t sample_count = stts->GetSampleCount(i);
    stts_sample_count += sample_count;
    info.timing.num_video_frames += sample_count;
    uint32_t sample_offset = stts->GetSampleOffset(i);
    for (uint32_t sample = 0; sample < sample_count; sample++) {
      // store the new stts value
      info.timing.stts_unit_list.push_back(sample_offset);
      // set the dts value of the next frame
      uint32_t dts_unit = last_dts_unit + sample_offset;
      float dts_sec = (float)dts_unit / timescale_hz;
      info.timing.dts_sec_list.push_back(dts_sec);
      // init the pts value of the next frame
      info.timing.pts_sec_list.push_back(dts_sec);
      last_dts_unit = dts_unit;
    }
    if (debug > 2) {
      fprintf(stdout, "stts::sample_count: %u ", sample_count);
      fprintf(stdout, "stts::sample_offset: %u ", sample_offset);
    }
  }
  // we need to remove the last element of the dts and pts lists, as we
  // set them pointing at the start of the next frame (inexistent)
  info.timing.dts_sec_list.pop_back();
  info.timing.pts_sec_list.pop_back();

  // 3. look for a ctts box
  std::shared_ptr<ISOBMFF::CTTS> ctts =
      stbl->GetTypedBox<ISOBMFF::CTTS>("ctts");
  if (ctts != nullptr) {
    float last_ctts_sample_offset_sec = 0.0;
    // 10.1. adjust pts list using ctts timestamp durations
    uint32_t ctts_sample_count = 0;
    uint32_t cur_video_frame = 0;
    for (unsigned int i = 0; i < ctts->GetEntryCount(); i++) {
      uint32_t sample_count = ctts->GetSampleCount(i);
      ctts_sample_count += sample_count;
      // update pts_sec_list
      int32_t sample_offset = ctts->GetSampleOffset(i);
      float sample_offset_sec = (float)sample_offset / timescale_hz;
      last_ctts_sample_offset_sec = sample_offset_sec;
      for (uint32_t sample = 0; sample < sample_count; sample++) {
        // store the new ctts value
        info.timing.ctts_unit_list.push_back(sample_offset);
        // update the pts value
        info.timing.pts_sec_list[cur_video_frame] += sample_offset_sec;
        ++cur_video_frame;
      }
      if (debug > 2) {
        fprintf(stdout, "ctts::sample_count: %u ", sample_count);
        fprintf(stdout, "ctts::sample_offset: %i ", sample_offset);
      }
    }
    // standard suggests that, if there are less ctts than actual samples,
    // the decoder reuses the latest ctts sample offset again and again
    while (cur_video_frame < stts_sample_count) {
      // update the pts value
      info.timing.pts_sec_list[cur_video_frame] += last_ctts_sample_offset_sec;
      ++cur_video_frame;
    }
    if (debug > 2) {
      printf("cur_video_frame: %u\n", cur_video_frame);
      printf("stts_sample_count: %u\n", stts_sample_count);
      printf("ctts_sample_count: %u\n", ctts_sample_count);
    }
  }

  return 0;
}

int get_keyframe_information(std::shared_ptr<ISOBMFF::ContainerBox> stbl,
                             struct IsobmffFileInformation &info, int debug) {
  // look for a stss box in the video track for key frames
  std::shared_ptr<ISOBMFF::STSS> stss =
      stbl->GetTypedBox<ISOBMFF::STSS>("stss");
  if (stss == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "warning: no /moov/trak/mdia/minf/stbl/stss in %s\n",
              info.filename.c_str());
    }
  } else {
    for (unsigned int i = 0; i < stss->GetEntryCount(); i++) {
      uint32_t sample_count = stss->GetSampleNumber(i);
      info.timing.keyframe_sample_number_list.push_back(sample_count);
    }
  }

  return 0;
}

// Function derives an (N-1)-element vector from an N-element vector by
// setting element i-th as:
//   out[i] = in[i] - in[i-1]
//
// The function will allocate all the N-1 elements.
void calculate_vector_deltas(const std::vector<float> in,
                             std::vector<float> &out) {
  out.clear();
  float last_val = -1.0;
  for (const auto &val : in) {
    if (last_val != -1.0) {
      out.push_back(val - last_val);
    }
    last_val = val;
  }
}

float calculate_median(std::vector<float> &vec) {
  std::sort(vec.begin(), vec.end());
  size_t n = vec.size();
  if (n % 2 == 0) {
    return (vec[n / 2 - 1] + vec[n / 2]) / 2.0f;
  } else {
    return vec[n / 2];
  }
}

float calculate_average(const std::vector<float> &vec) {
  return std::accumulate(vec.begin(), vec.end(), 0.0f) / vec.size();
}

float calculate_standard_deviation(const std::vector<float> &vec) {
  if (vec.size() < 2) {
    throw std::runtime_error("Input vector must have at least 2 elements");
  }

  float mean = calculate_average(vec);
  float sum_squares = 0.0f;
  for (const float &x : vec) {
    sum_squares += (x - mean) * (x - mean);
  }

  return std::sqrt(sum_squares / (vec.size() - 1));
}

// https://en.wikipedia.org/wiki/Median_absolute_deviation
float calculate_median_absolute_deviation(std::vector<float> &vec) {
  // \tilde(X): median(vec)
  float median = calculate_median(vec);
  // |Xi - \tilde(X)|: vector of absolute differences to the median
  std::vector<float> vec_abs_differences(vec.size());
  for (size_t i = 0; i < vec.size(); i++) {
    vec_abs_differences[i] = abs(vec[i] - median);
  }
  // MAD = median(|Xi - \tilde(X)|)
  float mad = calculate_median(vec_abs_differences);
  return mad;
}

int derive_timing_info(struct TimingInformation &timing, bool sort_by_pts,
                       int debug) {
  // 1. set the frame_num_orig_list vector
  timing.frame_num_orig_list.resize(timing.pts_sec_list.size());
  for (uint32_t i = 0; i < timing.pts_sec_list.size(); ++i) {
    timing.frame_num_orig_list[i] = i;
  }

  // 2. sort the frames by pts value
  if (sort_by_pts) {
    // sort frame_num_orig_list elements based on the values in pts_sec_list
    // TODO(chema): there should be a clear way to access the struct element
    const auto &pts_sec_list = timing.pts_sec_list;
    std::stable_sort(timing.frame_num_orig_list.begin(),
                     timing.frame_num_orig_list.end(),
                     [&pts_sec_list](int a, int b) {
                       return pts_sec_list[a] < pts_sec_list[b];
                     });
    // sort all the others based in the new order
    // 2.1. stts_unit_list
    std::vector<uint32_t> stts_unit_list_alt(timing.stts_unit_list.size());
    for (uint32_t i = 0; i < timing.stts_unit_list.size(); ++i) {
      stts_unit_list_alt[i] =
          timing.stts_unit_list[timing.frame_num_orig_list[i]];
    }
    timing.stts_unit_list = stts_unit_list_alt;
    // 2.2. ctts_unit_list
    std::vector<int32_t> ctts_unit_list_alt(timing.ctts_unit_list.size());
    for (uint32_t i = 0; i < timing.ctts_unit_list.size(); ++i) {
      ctts_unit_list_alt[i] =
          timing.ctts_unit_list[timing.frame_num_orig_list[i]];
    }
    timing.ctts_unit_list = ctts_unit_list_alt;
    // 2.3. dts_sec_list
    std::vector<float> dts_sec_list_alt(timing.dts_sec_list.size());
    for (uint32_t i = 0; i < timing.dts_sec_list.size(); ++i) {
      dts_sec_list_alt[i] = timing.dts_sec_list[timing.frame_num_orig_list[i]];
    }
    timing.dts_sec_list = dts_sec_list_alt;
    // 2.4. pts_sec_list
    std::vector<float> pts_sec_list_alt(timing.pts_sec_list.size());
    for (uint32_t i = 0; i < timing.pts_sec_list.size(); ++i) {
      pts_sec_list_alt[i] = timing.pts_sec_list[timing.frame_num_orig_list[i]];
    }
    timing.pts_sec_list = pts_sec_list_alt;
  }

  // 3. derived timing values
  // 3.1. calculate the duration (inter-frame distance)
  std::vector<float> pts_sec_duration_list;
  calculate_vector_deltas(timing.pts_sec_list, pts_sec_duration_list);
  // 3.2. calculate the duration average/median
  timing.pts_sec_duration_average = calculate_average(pts_sec_duration_list);
  timing.pts_sec_duration_median = calculate_median(pts_sec_duration_list);
  // 3.3. calculate the duration stddev and median absolute difference (MAD)
  timing.pts_sec_duration_stddev =
      calculate_standard_deviation(pts_sec_duration_list);
  timing.pts_sec_duration_mad =
      calculate_median_absolute_deviation(pts_sec_duration_list);

  // 4. derive pts duration values
  timing.pts_duration_sec_list.clear();
  for (unsigned int i = 1; i < timing.pts_sec_list.size(); ++i) {
    timing.pts_duration_sec_list.push_back(timing.pts_sec_list[i] -
                                           timing.pts_sec_list[i - 1]);
  }

  return 0;
}

void parse_avcc(std::shared_ptr<ISOBMFF::AVCC> avcc,
                struct FrameInformation &frame, int debug) {
  // define an hevc parser state
  h264nal::H264BitstreamParserState bitstream_parser_state;
  std::unique_ptr<h264nal::H264BitstreamParser::BitstreamState> bitstream;
  h264nal::ParsingOptions parsing_options;
  parsing_options.add_offset = false;
  parsing_options.add_length = false;
  parsing_options.add_parsed_length = false;
  parsing_options.add_checksum = false;
  parsing_options.add_resolution = false;

  // set default values
  frame.colour_primaries = -1;
  frame.transfer_characteristics = -1;
  frame.matrix_coeffs = -1;
  frame.video_full_range_flag = -1;

  // extract the SPS NAL Units
  for (const auto &sps : avcc->GetSequenceParameterSetNALUnits()) {
    std::vector<uint8_t> buffer = sps->GetData();
    auto nal_unit = h264nal::H264NalUnitParser::ParseNalUnit(
        buffer.data(), buffer.size(), &bitstream_parser_state, parsing_options);
    if (nal_unit == nullptr) {
      // cannot parse the NalUnit
      continue;
    }
    // look for valid SPS NAL units
    if ((nal_unit->nal_unit_payload->sps->sps_data
             ->vui_parameters_present_flag == 1) &&
        (nal_unit->nal_unit_payload->sps->sps_data->vui_parameters
             ->colour_description_present_flag == 1)) {
      frame.colour_primaries = nal_unit->nal_unit_payload->sps->sps_data
                                   ->vui_parameters->colour_primaries;
      frame.transfer_characteristics =
          nal_unit->nal_unit_payload->sps->sps_data->vui_parameters
              ->transfer_characteristics;
      frame.matrix_coeffs = nal_unit->nal_unit_payload->sps->sps_data
                                ->vui_parameters->matrix_coefficients;
      frame.video_full_range_flag = nal_unit->nal_unit_payload->sps->sps_data
                                        ->vui_parameters->video_full_range_flag;
    }
  }
}

void parse_hvcc(std::shared_ptr<ISOBMFF::HVCC> hvcc,
                struct FrameInformation &frame, int debug) {
  // define an hevc parser state
  h265nal::H265BitstreamParserState bitstream_parser_state;
  std::unique_ptr<h265nal::H265BitstreamParser::BitstreamState> bitstream;
  h265nal::ParsingOptions parsing_options;
  parsing_options.add_offset = false;
  parsing_options.add_length = false;
  parsing_options.add_parsed_length = false;
  parsing_options.add_checksum = false;
  parsing_options.add_resolution = false;

  // set default values
  frame.colour_primaries = -1;
  frame.transfer_characteristics = -1;
  frame.matrix_coeffs = -1;
  frame.video_full_range_flag = -1;

  // extract the NAL Units
  for (const auto &array : hvcc->GetArrays()) {
    // bool array_completeness = array->GetArrayCompleteness();
    uint8_t nal_unit_type = array->GetNALUnitType();
    for (const auto &data : array->GetNALUnits()) {
      std::vector<uint8_t> buffer = data->GetData();
      auto nal_unit = h265nal::H265NalUnitParser::ParseNalUnit(
          buffer.data(), buffer.size(), &bitstream_parser_state,
          parsing_options);
      if (nal_unit == nullptr) {
        // cannot parse the NalUnit
        continue;
      }
      // look for SPS NAL units
      if ((nal_unit_type == h265nal::NalUnitType::SPS_NUT) &&
          (nal_unit->nal_unit_payload->sps->vui_parameters_present_flag == 1) &&
          (nal_unit->nal_unit_payload->sps->vui_parameters
               ->colour_description_present_flag == 1)) {
        frame.colour_primaries =
            nal_unit->nal_unit_payload->sps->vui_parameters->colour_primaries;
        frame.transfer_characteristics =
            nal_unit->nal_unit_payload->sps->vui_parameters
                ->transfer_characteristics;
        frame.matrix_coeffs =
            nal_unit->nal_unit_payload->sps->vui_parameters->matrix_coeffs;
        frame.video_full_range_flag =
            nal_unit->nal_unit_payload->sps->vui_parameters
                ->video_full_range_flag;
      }
    }
  }
}

int get_frame_information(std::shared_ptr<ISOBMFF::ContainerBox> stbl,
                          struct IsobmffFileInformation &info, int debug) {
  // 1. look for a stsd container box
  std::shared_ptr<ISOBMFF::STSD> stsd =
      stbl->GetTypedBox<ISOBMFF::STSD>("stsd");
  if (stsd == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl/stsd in %s\n",
              info.filename.c_str());
    }
    return -1;
  }

  // 2. look for hvc1 or avc1 (container) boxes
  std::shared_ptr<ISOBMFF::HVC1> hvc1 =
      stsd->GetTypedBox<ISOBMFF::HVC1>("hvc1");
  std::shared_ptr<ISOBMFF::AVC1> avc1 =
      stsd->GetTypedBox<ISOBMFF::AVC1>("avc1");

  if (hvc1 != nullptr) {
    // 3. look for an hvcC box
    std::shared_ptr<ISOBMFF::HVCC> hvcc =
        hvc1->GetTypedBox<ISOBMFF::HVCC>("hvcC");
    if (hvcc == nullptr) {
      if (debug > 0) {
        fprintf(stderr,
                "error: no /moov/trak/mdia/minf/stbl/stsd/hvc1/hvcC in %s\n",
                info.filename.c_str());
      }
      return -1;
    }
    info.frame.type = "hvc1";
    info.frame.width2 = hvc1->GetWidth();
    info.frame.height2 = hvc1->GetHeight();
    info.frame.horizresolution = hvc1->GetHorizResolution();
    info.frame.vertresolution = hvc1->GetVertResolution();
    info.frame.depth = hvc1->GetDepth();
    info.frame.chroma_format = hvcc->GetChromaFormat();
    info.frame.bit_depth_luma = 8 + hvcc->GetBitDepthLumaMinus8();
    info.frame.bit_depth_chroma = 8 + hvcc->GetBitDepthChromaMinus8();
    info.frame.colour_primaries = -1;
    info.frame.transfer_characteristics = -1;
    info.frame.matrix_coeffs = -1;
    info.frame.video_full_range_flag = -1;
    parse_hvcc(hvcc, info.frame, debug);

  } else if (avc1 != nullptr) {
    // 4. look for an avcC box
    std::shared_ptr<ISOBMFF::AVCC> avcc =
        avc1->GetTypedBox<ISOBMFF::AVCC>("avcC");
    if (avcc == nullptr) {
      if (debug > 0) {
        fprintf(stderr,
                "error: no /moov/trak/mdia/minf/stbl/stsd/avc1/avcC in %s\n",
                info.filename.c_str());
      }
      return -1;
    }
    info.frame.type = "avc1";
    info.frame.width2 = avc1->GetWidth();
    info.frame.height2 = avc1->GetHeight();
    info.frame.horizresolution = avc1->GetHorizResolution();
    info.frame.vertresolution = avc1->GetVertResolution();
    info.frame.depth = avc1->GetDepth();
    info.frame.chroma_format = -1;
    info.frame.bit_depth_luma = -1;
    info.frame.bit_depth_chroma = -1;
    parse_avcc(avcc, info.frame, debug);

  } else {
    if (debug > 0) {
      fprintf(stderr,
              "error: no /moov/trak/mdia/minf/stbl/stsd/hvc1 or "
              "/moov/trak/mdia/minf/stbl/stsd/avc1 in %s\n",
              info.filename.c_str());
    }
    return -1;
  }

  return 0;
}

int get_frame_interframe_info(
    const struct IsobmffFileInformation &info, int *num_video_frames,
    std::vector<uint32_t> &frame_num_orig_list,
    std::vector<uint32_t> &stts_unit_list, std::vector<int32_t> &ctts_unit_list,
    std::vector<float> &dts_sec_list, std::vector<float> &pts_sec_list,
    std::vector<float> &pts_duration_sec_list, bool sort_by_pts, int debug) {
  // get the list of frame durations
  *num_video_frames = info.timing.num_video_frames;
  frame_num_orig_list = info.timing.frame_num_orig_list;
  stts_unit_list = info.timing.stts_unit_list;
  ctts_unit_list = info.timing.ctts_unit_list;
  dts_sec_list = info.timing.dts_sec_list;
  pts_sec_list = info.timing.pts_sec_list;
  pts_duration_sec_list = info.timing.pts_duration_sec_list;

  return 0;
}

int get_frame_drop_info(const struct IsobmffFileInformation &info,
                        int *num_video_frames, float *frame_rate_fps_median,
                        float *frame_rate_fps_average,
                        float *frame_rate_fps_stddev, int *frame_drop_count,
                        float *frame_drop_ratio,
                        float *normalized_frame_drop_average_length,
                        const std::vector<float> percentile_list,
                        std::vector<float> &frame_drop_length_percentile_list,
                        std::vector<int> consecutive_list,
                        std::vector<long int> &frame_drop_length_consecutive,
                        int debug) {
  // 0. get the list of inter-frame timestamp distances from the pts_sec_list
  *num_video_frames = info.timing.num_video_frames;
  std::vector<float> pts_sec_duration_list;
  calculate_vector_deltas(info.timing.pts_sec_list, pts_sec_duration_list);

  if (debug > 1) {
    for (const auto &pts_sec_duration : pts_sec_duration_list) {
      fprintf(stdout, "%f,", pts_sec_duration);
    }
    fprintf(stdout, "\n");
  }

  // 1. calculate the inter-frame distance statistics
  // 1.1. get the list of frame rates
  std::vector<float> frame_rate_fps_list(pts_sec_duration_list.size());
  std::transform(pts_sec_duration_list.begin(), pts_sec_duration_list.end(),
                 frame_rate_fps_list.begin(), [](float val) {
                   // Handle division by zero
                   return val != 0.0f ? 1.0f / static_cast<float>(val) : 0.0f;
                 });
  // 1.2. median
  sort(frame_rate_fps_list.begin(), frame_rate_fps_list.end());
  *frame_rate_fps_median =
      frame_rate_fps_list[frame_rate_fps_list.size() / 2 - 1];
  sort(pts_sec_duration_list.begin(), pts_sec_duration_list.end());
  float pts_sec_duration_median =
      pts_sec_duration_list[pts_sec_duration_list.size() / 2 - 1];
  // 1.3. average
  *frame_rate_fps_average =
      (1.0 * std::accumulate(frame_rate_fps_list.begin(),
                             frame_rate_fps_list.end(), 0.0)) /
      frame_rate_fps_list.size();
  // 1.4. stddev
  // vmas := value minus average square
  std::vector<float> frame_rate_fps_vmas_list(frame_rate_fps_list.size());
  std::transform(frame_rate_fps_list.begin(), frame_rate_fps_list.end(),
                 frame_rate_fps_vmas_list.begin(), [=](float val) {
                   return (val - (*frame_rate_fps_average)) *
                          (val - (*frame_rate_fps_average));
                 });
  double frame_rate_fps_square_sum = std::accumulate(
      frame_rate_fps_vmas_list.begin(), frame_rate_fps_vmas_list.end(), 0.0);
  *frame_rate_fps_stddev =
      std::sqrt(frame_rate_fps_square_sum / frame_rate_fps_vmas_list.size());

  // 2. calculate the threshold to consider frame drop: This should be 2
  // times the median, minus a factor
  double FACTOR = 0.75;
  float pts_sec_duration_threshold = pts_sec_duration_median * FACTOR * 2;

  // 3. get the list of all the drops (absolute inter-frame values)
  std::vector<float> drop_length_sec_list;
  for (const auto &pts_sec_duration : pts_sec_duration_list) {
    if (pts_sec_duration > pts_sec_duration_threshold) {
      drop_length_sec_list.push_back(pts_sec_duration);
    }
  }
  // drop_length_sec_list: {0.6668900000000022, 0.10025600000000168, ...}

  // 4. sum all the drops, but adding only the length over 1x frame time
  float drop_length_sec_list_sum = 0.0;
  for (const auto &drop_length_sec : drop_length_sec_list) {
    drop_length_sec_list_sum += drop_length_sec;
  }
  float drop_length_duration_sec =
      drop_length_sec_list_sum -
      pts_sec_duration_median * drop_length_sec_list.size();
  // drop_length_duration_sec: sum({33.35900000000022, 66.92600000000168, ...})

  // 5. get the total duration as the sum of all the inter-frame distances
  float total_duration_sec = 0.0;
  for (const auto &pts_sec_duration : pts_sec_duration_list) {
    total_duration_sec += pts_sec_duration;
  }

  // 6. calculate frame drop ratio as extra drop length over total duration
  *frame_drop_ratio = drop_length_duration_sec / total_duration_sec;
  *frame_drop_count = int((*frame_drop_ratio) * (*num_video_frames));

  // 7. calculate average drop length, normalized to framerate. Note that
  // a single frame drop is a normalized frame drop length of 2. When
  // frame drops are uncorrelated, the normalized average drop length
  // should be close to 2
  *normalized_frame_drop_average_length = 0.0;
  if (drop_length_sec_list.size() > 0) {
    float frame_drop_average_length =
        drop_length_sec_list_sum / drop_length_sec_list.size();
    *normalized_frame_drop_average_length =
        (frame_drop_average_length / pts_sec_duration_median);
  }

  // 8. calculate percentile list
  frame_drop_length_percentile_list.clear();
  if (drop_length_sec_list.size() > 0) {
    sort(drop_length_sec_list.begin(), drop_length_sec_list.end());
    for (const float &percentile : percentile_list) {
      int position = (percentile / 100.0) * drop_length_sec_list.size();
      float frame_drop_length_percentile =
          drop_length_sec_list[position] / pts_sec_duration_median;
      frame_drop_length_percentile_list.push_back(frame_drop_length_percentile);
    }
  } else {
    for (const float &_ : percentile_list) {
      frame_drop_length_percentile_list.push_back(0.0);
    }
  }

  // 9. calculate consecutive list
  frame_drop_length_consecutive.clear();
  for (int _ : consecutive_list) {
    frame_drop_length_consecutive.push_back(0);
  }
  if (drop_length_sec_list.size() > 0) {
    for (const auto &drop_length_sec : drop_length_sec_list) {
      float drop_length = drop_length_sec / pts_sec_duration_median;
      for (unsigned int i = 0; i < consecutive_list.size(); i++) {
        if (drop_length >= consecutive_list[i]) {
          frame_drop_length_consecutive[i]++;
        }
      }
    }
  }

  return 0;
}

int get_video_freeze_info(const struct IsobmffFileInformation &info,
                          bool *video_freeze, float *audio_video_ratio,
                          float *duration_video_sec, float *duration_audio_sec,
                          uint32_t *timescale_video_hz,
                          uint32_t *timescale_audio_hz,
                          float *pts_sec_duration_average,
                          float *pts_sec_duration_median,
                          float *pts_sec_duration_stddev,
                          float *pts_sec_duration_mad, int debug) {
  // 0. init values
  *duration_video_sec = info.timing.duration_video_sec;
  *duration_audio_sec = info.timing.duration_audio_sec;
  *timescale_video_hz = info.timing.timescale_video_hz;
  *timescale_audio_hz = info.timing.timescale_audio_hz;
  *pts_sec_duration_average = info.timing.pts_sec_duration_average;
  *pts_sec_duration_median = info.timing.pts_sec_duration_median;
  *pts_sec_duration_stddev = info.timing.pts_sec_duration_stddev;
  *pts_sec_duration_mad = info.timing.pts_sec_duration_mad;

  // 1. check both audio and video tracks, and video track at least 2 seconds
  if (*duration_video_sec == -1.0) {
    fprintf(stderr, "error: no video track in %s\n", info.filename.c_str());
    return -1;
  }
  if (*duration_audio_sec == -1.0) {
    fprintf(stderr, "warn: no audio track in %s\n", info.filename.c_str());
    return 0;
  }
  if (*duration_video_sec < 2.0) {
    fprintf(stderr, "error: video track too short %s (%f seconds)\n",
            info.filename.c_str(), *duration_video_sec);
    return -1;
  }

  // 2. calculate audio to video ratio
  *audio_video_ratio = *duration_audio_sec / *duration_video_sec;
  *video_freeze = *audio_video_ratio > MAX_AUDIO_VIDEO_RATIO;
  return 0;
}

int get_video_structure_info(const struct IsobmffFileInformation &info,
                             int *num_video_frames, int *num_video_keyframes,
                             int debug) {
  // 0. init values
  *num_video_frames = info.timing.num_video_frames;

  // 1. count the number of video keyframes from stss (0 if no stss)
  *num_video_keyframes = info.timing.keyframe_sample_number_list.size();

  return 0;
}

int get_video_generic_info(const struct IsobmffFileInformation &info,
                           int *filesize, float *bitrate_bps, int *width,
                           int *height, std::string &type, int *width2,
                           int *height2, int *horizresolution,
                           int *vertresolution, int *depth, int *chroma_format,
                           int *bit_depth_luma, int *bit_depth_chroma,
                           int *video_full_range_flag, int *colour_primaries,
                           int *transfer_characteristics, int *matrix_coeffs,
                           int debug) {
  // 1. convert width and height to integers
  type = info.frame.type;
  *width = static_cast<int>(info.frame.width);
  *height = static_cast<int>(info.frame.height);
  *width2 = info.frame.width2;
  *height2 = info.frame.height2;
  *horizresolution = info.frame.horizresolution;
  *vertresolution = info.frame.vertresolution;
  *depth = info.frame.depth;
  *chroma_format = info.frame.chroma_format;
  *bit_depth_luma = info.frame.bit_depth_luma;
  *bit_depth_chroma = info.frame.bit_depth_chroma;
  *video_full_range_flag = info.frame.video_full_range_flag;
  *colour_primaries = info.frame.colour_primaries;
  *transfer_characteristics = info.frame.transfer_characteristics;
  *matrix_coeffs = info.frame.matrix_coeffs;

  // 2. get file size
  struct stat stat_buf;
  int rc = stat(info.filename.c_str(), &stat_buf);

  // 3. get bitrate_bps
  *bitrate_bps = -1.0;
  *filesize = -1;
  if (rc == 0) {
    *filesize = stat_buf.st_size;
    *bitrate_bps =
        8.0 * (float)(*filesize) / (float)info.timing.duration_video_sec;
  }

  return 0;
}
