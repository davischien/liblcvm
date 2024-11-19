// liblcvm: Low-Complexity Video Metrics Library.
// A library to detect frame dups and video freezes.

// A show case of using [ISOBMFF](https://github.com/DigiDNA/ISOBMFF) to
// detect frame dups and video freezes in ISOBMFF files.

#include <h265_bitstream_parser.h>
#include <h265_common.h>
#include <h265_nal_unit_parser.h>
#include <stdint.h>  // for uint32_t, uint64_t

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

#define MAX_AUDIO_VIDEO_RATIO 0.05

int get_liblcvm_version(std::string &version) {
  version = PROJECT_VER;
  return 0;
}

struct TimingInformation {
  int num_video_frames;
  float duration_video_sec;
  float duration_audio_sec;
  std::vector<float> delta_timestamp_sec_list;
  std::vector<uint32_t> keyframe_sample_number_list;
} TimingInformation;

int get_timing_information(const char *infile,
                           struct TimingInformation *timing_information,
                           int debug) {
  // 1. parse the input file
  ISOBMFF::Parser parser;
  try {
    parser.Parse(infile);
  } catch (std::runtime_error &e) {
    return -1;
  }
  std::shared_ptr<ISOBMFF::File> file = parser.GetFile();
  if (file == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no file in %s\n", infile);
    }
    return -1;
  }

  // 2. look for a moov container box
  std::shared_ptr<ISOBMFF::ContainerBox> moov =
      file->GetTypedBox<ISOBMFF::ContainerBox>("moov");
  if (moov == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no /moov in %s\n", infile);
    }
    return -1;
  }

  // 3. look for trak container boxes
  timing_information->duration_video_sec = -1.0;
  timing_information->duration_audio_sec = -1.0;
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
        fprintf(stderr, "error: no /moov/trak/mdia in %s\n", infile);
      }
      return -1;
    }

    // 5. look for a hdlr box
    std::shared_ptr<ISOBMFF::HDLR> hdlr =
        mdia->GetTypedBox<ISOBMFF::HDLR>("hdlr");
    if (hdlr == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/hdlr in %s\n", infile);
      }
      return -1;
    }
    std::string handler_type = hdlr->GetHandlerType();

    // 6. look for a mdhd box
    std::shared_ptr<ISOBMFF::MDHD> mdhd =
        mdia->GetTypedBox<ISOBMFF::MDHD>("mdhd");
    if (mdhd == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/mdhd in %s\n", infile);
      }
      return -1;
    }
    uint32_t timescale = mdhd->GetTimescale();
    uint64_t duration = mdhd->GetDuration();
    float duration_sec = (float)duration / timescale;
    if (debug > 1) {
      fprintf(stdout, "-> handler_type: %s ", handler_type.c_str());
      fprintf(stdout, "timescale: %u ", timescale);
      fprintf(stdout, "duration: %lu ", duration);
      fprintf(stdout, "duration_sec: %f\n", duration_sec);
    }
    if (handler_type.compare("soun") == 0) {
      timing_information->duration_audio_sec = duration_sec;
    } else if (handler_type.compare("vide") == 0) {
      timing_information->duration_video_sec = duration_sec;
    }

    // only keep video processing
    if (handler_type.compare("vide") != 0) {
      continue;
    }

    // 7. look for a minf container box
    std::shared_ptr<ISOBMFF::ContainerBox> minf =
        mdia->GetTypedBox<ISOBMFF::ContainerBox>("minf");
    if (minf == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf in %s\n", infile);
      }
      return -1;
    }

    // 8. look for a stbl container box
    std::shared_ptr<ISOBMFF::ContainerBox> stbl =
        minf->GetTypedBox<ISOBMFF::ContainerBox>("stbl");
    if (stbl == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl in %s\n", infile);
      }
      return -1;
    }

    // 9. look for a stts box
    std::shared_ptr<ISOBMFF::STTS> stts =
        stbl->GetTypedBox<ISOBMFF::STTS>("stts");
    if (stts == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl/stts in %s\n",
                infile);
      }
      return -1;
    }

    // 10. gather the inter-frame timestamp deltas
    timing_information->delta_timestamp_sec_list.clear();
    timing_information->num_video_frames = 0;
    for (unsigned int i = 0; i < stts->GetEntryCount(); i++) {
      uint32_t sample_count = stts->GetSampleCount(i);
      timing_information->num_video_frames += sample_count;
      uint32_t sample_offset = stts->GetSampleOffset(i);
      float sample_offset_sec = (float)sample_offset / timescale;
      for (uint32_t sample = 0; sample < sample_count; sample++) {
        timing_information->delta_timestamp_sec_list.push_back(
            sample_offset_sec);
      }
      if (debug > 2) {
        fprintf(stdout, "sample_count: %u ", sample_count);
        fprintf(stdout, "sample_offset: %u ", sample_offset);
        fprintf(stdout, "sample_offset_sec: %f\n", sample_offset_sec);
      }
    }

    // 11. look for a stss box in the video track
    if (handler_type.compare("vide") == 0) {
      std::shared_ptr<ISOBMFF::STSS> stss =
          stbl->GetTypedBox<ISOBMFF::STSS>("stss");
      if (stss == nullptr) {
        if (debug > 0) {
          fprintf(stderr, "warning: no /moov/trak/mdia/minf/stbl/stss in %s\n",
                  infile);
        }
      } else {
        timing_information->keyframe_sample_number_list.clear();
        for (unsigned int i = 0; i < stss->GetEntryCount(); i++) {
          uint32_t sample_count = stss->GetSampleNumber(i);
          timing_information->keyframe_sample_number_list.push_back(
              sample_count);
        }
      }
    }
  }

  return 0;
}

struct FrameInformation {
  float width;
  float height;
  std::string type;
  int width2;
  int height2;
  int horizresolution;
  int vertresolution;
  int depth;
  int chroma_format;
  int bit_depth_luma;
  int bit_depth_chroma;
  int video_full_range_flag;
  int colour_primaries;
  int transfer_characteristics;
  int matrix_coeffs;
} FrameInformation;

void parse_hvcc(std::shared_ptr<ISOBMFF::HVCC> hvcc,
                struct FrameInformation *frame_information, int debug) {
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
  frame_information->colour_primaries = -1;
  frame_information->transfer_characteristics = -1;
  frame_information->matrix_coeffs = -1;
  frame_information->video_full_range_flag = -1;

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
        frame_information->colour_primaries =
            nal_unit->nal_unit_payload->sps->vui_parameters->colour_primaries;
        frame_information->transfer_characteristics =
            nal_unit->nal_unit_payload->sps->vui_parameters
                ->transfer_characteristics;
        frame_information->matrix_coeffs =
            nal_unit->nal_unit_payload->sps->vui_parameters->matrix_coeffs;
        frame_information->video_full_range_flag =
            nal_unit->nal_unit_payload->sps->vui_parameters
                ->video_full_range_flag;
      }
    }
  }
}

int get_frame_information(const char *infile,
                          struct FrameInformation *frame_information,
                          int debug) {
  // 1. parse the input file
  ISOBMFF::Parser parser;
  try {
    parser.Parse(infile);
  } catch (std::runtime_error &e) {
    return -1;
  }
  std::shared_ptr<ISOBMFF::File> file = parser.GetFile();
  if (file == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no file in %s\n", infile);
    }
    return -1;
  }

  // 2. look for a moov container box
  std::shared_ptr<ISOBMFF::ContainerBox> moov =
      file->GetTypedBox<ISOBMFF::ContainerBox>("moov");
  if (moov == nullptr) {
    if (debug > 0) {
      fprintf(stderr, "error: no /moov in %s\n", infile);
    }
    return -1;
  }

  // 3. look for trak container boxes
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
        fprintf(stderr, "error: no /moov/trak/mdia in %s\n", infile);
      }
      return -1;
    }

    // 5. look for a hdlr box
    std::shared_ptr<ISOBMFF::HDLR> hdlr =
        mdia->GetTypedBox<ISOBMFF::HDLR>("hdlr");
    if (hdlr == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/hdlr in %s\n", infile);
      }
      return -1;
    }
    std::string handler_type = hdlr->GetHandlerType();

    // only keep video processing
    if (handler_type.compare("vide") != 0) {
      continue;
    }

    // 6. look for a tkhd box
    std::shared_ptr<ISOBMFF::TKHD> tkhd =
        trak->GetTypedBox<ISOBMFF::TKHD>("tkhd");
    if (tkhd == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/tkhd in %s\n", infile);
      }
      return -1;
    }
    frame_information->width = tkhd->GetWidth();
    frame_information->height = tkhd->GetHeight();

    // 7. look for a minf container box
    std::shared_ptr<ISOBMFF::ContainerBox> minf =
        mdia->GetTypedBox<ISOBMFF::ContainerBox>("minf");
    if (minf == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf in %s\n", infile);
      }
      return -1;
    }

    // 8. look for a stbl container box
    std::shared_ptr<ISOBMFF::ContainerBox> stbl =
        minf->GetTypedBox<ISOBMFF::ContainerBox>("stbl");
    if (stbl == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl in %s\n", infile);
      }
      return -1;
    }

    // 9. look for a stsd container box
    std::shared_ptr<ISOBMFF::STSD> stsd =
        stbl->GetTypedBox<ISOBMFF::STSD>("stsd");
    if (stsd == nullptr) {
      if (debug > 0) {
        fprintf(stderr, "error: no /moov/trak/mdia/minf/stbl/stsd in %s\n",
                infile);
      }
      return -1;
    }

    // 10. look for hvc1 or avc1 (container) boxes
    std::shared_ptr<ISOBMFF::HVC1> hvc1 =
        stsd->GetTypedBox<ISOBMFF::HVC1>("hvc1");
    std::shared_ptr<ISOBMFF::AVC1> avc1 =
        stsd->GetTypedBox<ISOBMFF::AVC1>("avc1");

    if (hvc1 != nullptr) {
      // 11.1. look for an hvcC box
      std::shared_ptr<ISOBMFF::HVCC> hvcc =
          hvc1->GetTypedBox<ISOBMFF::HVCC>("hvcC");
      if (hvcc == nullptr) {
        if (debug > 0) {
          fprintf(stderr,
                  "error: no /moov/trak/mdia/minf/stbl/stsd/hvc1/hvcC in %s\n",
                  infile);
        }
        return -1;
      }
      frame_information->type = "hvc1";
      frame_information->width2 = hvc1->GetWidth();
      frame_information->height2 = hvc1->GetHeight();
      frame_information->horizresolution = hvc1->GetHorizResolution();
      frame_information->vertresolution = hvc1->GetVertResolution();
      frame_information->depth = hvc1->GetDepth();
      frame_information->chroma_format = hvcc->GetChromaFormat();
      frame_information->bit_depth_luma = 8 + hvcc->GetBitDepthLumaMinus8();
      frame_information->bit_depth_chroma = 8 + hvcc->GetBitDepthChromaMinus8();
      frame_information->colour_primaries = -1;
      frame_information->transfer_characteristics = -1;
      frame_information->matrix_coeffs = -1;
      frame_information->video_full_range_flag = -1;
      parse_hvcc(hvcc, frame_information, debug);

    } else if (avc1 != nullptr) {
      // 11.2. look for an avcC box
      std::shared_ptr<ISOBMFF::AVCC> avcc =
          avc1->GetTypedBox<ISOBMFF::AVCC>("avcC");
      if (avcc == nullptr) {
        if (debug > 0) {
          fprintf(stderr,
                  "error: no /moov/trak/mdia/minf/stbl/stsd/avc1/avcC in %s\n",
                  infile);
        }
        return -1;
      }
      frame_information->type = "avc1";
      frame_information->width2 = avc1->GetWidth();
      frame_information->height2 = avc1->GetHeight();
      frame_information->horizresolution = avc1->GetHorizResolution();
      frame_information->vertresolution = avc1->GetVertResolution();
      frame_information->depth = avc1->GetDepth();
      frame_information->chroma_format = -1;
      frame_information->bit_depth_luma = -1;
      frame_information->bit_depth_chroma = -1;

    } else {
      if (debug > 0) {
        fprintf(stderr,
                "error: no /moov/trak/mdia/minf/stbl/stsd/hvc1 or "
                "/moov/trak/mdia/minf/stbl/stsd/avc1 in %s\n",
                infile);
      }
      return -1;
    }
  }

  return 0;
}

int get_frame_interframe_info(const char *infile, int *num_video_frames,
                              std::vector<float> &delta_timestamp_sec_list,
                              int debug) {
  // get the list of inter-frame timestamp distances.
  struct TimingInformation timing_information;
  if (get_timing_information(infile, &timing_information, debug) < 0) {
    return -1;
  }
  *num_video_frames = timing_information.num_video_frames;
  delta_timestamp_sec_list = timing_information.delta_timestamp_sec_list;
  return 0;
}

int get_frame_drop_info(const char *infile, int *num_video_frames,
                        float *frame_rate_fps_median,
                        float *frame_rate_fps_average,
                        float *frame_rate_fps_stddev, int *frame_drop_count,
                        float *frame_drop_ratio,
                        float *normalized_frame_drop_average_length,
                        const std::vector<float> percentile_list,
                        std::vector<float> &frame_drop_length_percentile_list,
                        std::vector<int> consecutive_list,
                        std::vector<long int> &frame_drop_length_consecutive,
                        int debug) {
  // 0. get the list of inter-frame timestamp distances.
  struct TimingInformation timing_information;
  if (get_timing_information(infile, &timing_information, debug) < 0) {
    return -1;
  }
  *num_video_frames = timing_information.num_video_frames;

  if (debug > 1) {
    for (const auto &delta_timestamp_sec :
         timing_information.delta_timestamp_sec_list) {
      fprintf(stdout, "%f,", delta_timestamp_sec);
    }
    fprintf(stdout, "\n");
  }

  // 1. calculate the inter-frame distance statistics
  // 1.1. get the list of frame rates
  std::vector<float> frame_rate_fps_list(
      timing_information.delta_timestamp_sec_list.size());
  std::transform(timing_information.delta_timestamp_sec_list.begin(),
                 timing_information.delta_timestamp_sec_list.end(),
                 frame_rate_fps_list.begin(), [](float val) {
                   // Handle division by zero
                   return val != 0.0f ? 1.0f / static_cast<float>(val) : 0.0f;
                 });
  // 1.2. median
  sort(frame_rate_fps_list.begin(), frame_rate_fps_list.end());
  *frame_rate_fps_median =
      frame_rate_fps_list[frame_rate_fps_list.size() / 2 - 1];
  sort(timing_information.delta_timestamp_sec_list.begin(),
       timing_information.delta_timestamp_sec_list.end());
  float delta_timestamp_sec_median =
      timing_information.delta_timestamp_sec_list
          [timing_information.delta_timestamp_sec_list.size() / 2 - 1];
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
  float delta_timestamp_sec_threshold = delta_timestamp_sec_median * FACTOR * 2;

  // 3. get the list of all the drops (absolute inter-frame values)
  std::vector<float> drop_length_sec_list;
  for (const auto &delta_timestamp_sec :
       timing_information.delta_timestamp_sec_list) {
    if (delta_timestamp_sec > delta_timestamp_sec_threshold) {
      drop_length_sec_list.push_back(delta_timestamp_sec);
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
      delta_timestamp_sec_median * drop_length_sec_list.size();
  // drop_length_duration_sec: sum({33.35900000000022, 66.92600000000168, ...})

  // 5. get the total duration as the sum of all the inter-frame distances
  float total_duration_sec = 0.0;
  for (const auto &delta_timestamp_sec :
       timing_information.delta_timestamp_sec_list) {
    total_duration_sec += delta_timestamp_sec;
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
        (frame_drop_average_length / delta_timestamp_sec_median);
  }

  // 8. calculate percentile list
  frame_drop_length_percentile_list.clear();
  if (drop_length_sec_list.size() > 0) {
    sort(drop_length_sec_list.begin(), drop_length_sec_list.end());
    for (const float &percentile : percentile_list) {
      int position = (percentile / 100.0) * drop_length_sec_list.size();
      float frame_drop_length_percentile =
          drop_length_sec_list[position] / delta_timestamp_sec_median;
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
      float drop_length = drop_length_sec / delta_timestamp_sec_median;
      for (unsigned int i = 0; i < consecutive_list.size(); i++) {
        if (drop_length >= consecutive_list[i]) {
          frame_drop_length_consecutive[i]++;
        }
      }
    }
  }

  return 0;
}

int get_video_freeze_info(const char *infile, bool *video_freeze,
                          float *audio_video_ratio, float *duration_video_sec,
                          float *duration_audio_sec, int debug) {
  // 0. get timing information
  struct TimingInformation timing_information;
  if (get_timing_information(infile, &timing_information, debug) < 0) {
    return -1;
  }
  *duration_video_sec = timing_information.duration_video_sec;
  *duration_audio_sec = timing_information.duration_audio_sec;

  // 1. check both audio and video tracks, and video track at least 2 seconds
  if (*duration_video_sec == -1.0) {
    fprintf(stderr, "error: no video track in %s\n", infile);
    return -1;
  }
  if (*duration_audio_sec == -1.0) {
    fprintf(stderr, "warn: no audio track in %s\n", infile);
    return 0;
  }
  if (*duration_video_sec < 2.0) {
    fprintf(stderr, "error: video track too short %s (%f seconds)\n", infile,
            *duration_video_sec);
    return -1;
  }

  // 2. calculate audio to video ratio
  *audio_video_ratio =
      (*duration_audio_sec - *duration_video_sec) / *duration_audio_sec;
  *video_freeze = *audio_video_ratio > MAX_AUDIO_VIDEO_RATIO;
  return 0;
}

int get_video_structure_info(const char *infile, int *num_video_frames,
                             int *num_video_keyframes, int debug) {
  // 0. get timing information
  struct TimingInformation timing_information;
  if (get_timing_information(infile, &timing_information, debug) < 0) {
    return -1;
  }
  *num_video_frames = timing_information.num_video_frames;

  // 1. count the number of video keyframes from stss (0 if no stss)
  *num_video_keyframes = timing_information.keyframe_sample_number_list.size();

  return 0;
}

int get_video_generic_info(const char *infile, int *width, int *height,
                           std::string &type, int *width2, int *height2,
                           int *horizresolution, int *vertresolution,
                           int *depth, int *chroma_format, int *bit_depth_luma,
                           int *bit_depth_chroma, int *video_full_range_flag,
                           int *colour_primaries, int *transfer_characteristics,
                           int *matrix_coeffs, int debug) {
  // 0. get frame information
  struct FrameInformation frame_information;
  if (get_frame_information(infile, &frame_information, debug) < 0) {
    return -1;
  }

  // 1. convert width and height to integers
  type = frame_information.type;
  *width = static_cast<int>(frame_information.width);
  *height = static_cast<int>(frame_information.height);
  *width2 = frame_information.width2;
  *height2 = frame_information.height2;
  *horizresolution = frame_information.horizresolution;
  *vertresolution = frame_information.vertresolution;
  *depth = frame_information.depth;
  *chroma_format = frame_information.chroma_format;
  *bit_depth_luma = frame_information.bit_depth_luma;
  *bit_depth_chroma = frame_information.bit_depth_chroma;
  *video_full_range_flag = frame_information.video_full_range_flag;
  *colour_primaries = frame_information.colour_primaries;
  *transfer_characteristics = frame_information.transfer_characteristics;
  *matrix_coeffs = frame_information.matrix_coeffs;

  return 0;
}
