// Copyright 2023 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

#include "combine_command.h"

#include <cmath>

#include "avif/avif_cxx.h"
#include "imageio.h"

namespace avif {

CombineCommand::CombineCommand()
    : ProgramCommand("combine",
                     "Create an AVIF image with a gain map from a base image "
                     "and an alternate image") {
  argparse_.add_argument(arg_base_filename_, "base_image")
      .help(
          "The base image, that will be shown by viewers that don't support "
          "gain maps");
  argparse_.add_argument(arg_alternate_filename_, "alternate_image")
      .help("The alternate image, the result of fully applying the gain map");
  argparse_.add_argument(arg_output_filename_, "output_image.avif");
  argparse_.add_argument(arg_downscaling_, "--downscaling")
      .help("Downscaling factor for the gain map")
      .default_value("1");
  argparse_.add_argument(arg_gain_map_quality_, "--qgain-map")
      .help("Quality for the gain map (0-100, where 100 is lossless)")
      .default_value("60");
  argparse_.add_argument(arg_gain_map_depth_, "--depth-gain-map")
      .choices({"8", "10", "12"})
      .help("Output depth for the gain map")
      .default_value("8");
  argparse_
      .add_argument<int, PixelFormatConverter>(arg_gain_map_pixel_format_,
                                               "--yuv-gain-map")
      .choices({"444", "422", "420", "400"})
      .help("Output format for the gain map")
      .default_value("444");
  argparse_.add_argument(arg_max_headroom_, "--max-headroom")
      .help(
          "Maximum value for the base image HDR headroom and alternate "
          "image HDR headroom. Overrides the default headroom values computed "
          "from the image's content if they are larger than this maximum. Use "
          "0 for no maximum. "
          "E.g. assuming one of the two images is SDR and the "
          "other is HDR, the full HDR image (i.e. without tone mapping to SDR "
          "using the gain map) will be shown for displays with at least this "
          "amount of HDR headroom.")
      .default_value("4.0");
  argparse_
      .add_argument<CicpValues, CicpConverter>(arg_base_cicp_, "--cicp-base")
      .help(
          "Set or override the CICP values for the base image, expressed as "
          "P/T/M where P = color primaries, T = transfer characteristics, "
          "M = matrix coefficients.");
  argparse_
      .add_argument<CicpValues, CicpConverter>(arg_alternate_cicp_,
                                               "--cicp-alternate")
      .help(
          "Set or override the CICP values for the alternate image, expressed "
          "as P/T/M  where P = color primaries, T = transfer characteristics, "
          "M = matrix coefficients.");
  arg_image_encode_.Init(argparse_, /*can_have_alpha=*/true);
  arg_image_read_.Init(argparse_);
}

avifResult CombineCommand::Run() {
  const avifPixelFormat pixel_format =
      static_cast<avifPixelFormat>(arg_image_read_.pixel_format.value());
  const avifPixelFormat gain_map_pixel_format =
      static_cast<avifPixelFormat>(arg_gain_map_pixel_format_.value());

  ImagePtr base_image(avifImageCreateEmpty());
  ImagePtr alternate_image(avifImageCreateEmpty());
  if (base_image == nullptr || alternate_image == nullptr) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  }
  avifResult result =
      ReadImage(base_image.get(), arg_base_filename_, pixel_format,
                arg_image_read_.depth, arg_image_read_.ignore_profile);
  if (result != AVIF_RESULT_OK) {
    std::cout << "Failed to read base image: " << avifResultToString(result)
              << "\n";
    return result;
  }
  if (arg_base_cicp_.provenance() == argparse::Provenance::SPECIFIED) {
    base_image->colorPrimaries = arg_base_cicp_.value().color_primaries;
    base_image->transferCharacteristics =
        arg_base_cicp_.value().transfer_characteristics;
    base_image->matrixCoefficients = arg_base_cicp_.value().matrix_coefficients;
  }

  result =
      ReadImage(alternate_image.get(), arg_alternate_filename_, pixel_format,
                arg_image_read_.depth, arg_image_read_.ignore_profile);
  if (result != AVIF_RESULT_OK) {
    std::cout << "Failed to read alternate image: "
              << avifResultToString(result) << "\n";
    return result;
  }
  if (arg_alternate_cicp_.provenance() == argparse::Provenance::SPECIFIED) {
    alternate_image->colorPrimaries =
        arg_alternate_cicp_.value().color_primaries;
    alternate_image->transferCharacteristics =
        arg_alternate_cicp_.value().transfer_characteristics;
    alternate_image->matrixCoefficients =
        arg_alternate_cicp_.value().matrix_coefficients;
  }

  const uint32_t downscaling = std::max<int>(1, arg_downscaling_);
  const uint32_t rounding = downscaling / 2;
  const uint32_t gain_map_width =
      std::max((base_image->width + rounding) / downscaling, 1u);
  const uint32_t gain_map_height =
      std::max((base_image->height + rounding) / downscaling, 1u);
  std::cout << "Creating a gain map of size " << gain_map_width << " x "
            << gain_map_height << "\n";

  base_image->gainMap = avifGainMapCreate();
  base_image->gainMap->image =
      avifImageCreate(gain_map_width, gain_map_height, arg_gain_map_depth_,
                      gain_map_pixel_format);
  if (base_image->gainMap->image == nullptr) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  }
  avifDiagnostics diag;
  result = avifImageComputeGainMap(base_image.get(), alternate_image.get(),
                                   base_image->gainMap, &diag);
  if (result != AVIF_RESULT_OK) {
    std::cout << "Failed to compute gain map: " << avifResultToString(result)
              << " (" << diag.error << ")\n";
    return result;
  }

  if (arg_max_headroom_.value() > 0) {
    if (arg_max_headroom_.value() * base_image->gainMap->baseHdrHeadroom.d <
        base_image->gainMap->baseHdrHeadroom.n) {
      if (!avifDoubleToUnsignedFraction(
              arg_max_headroom_.value(),
              &base_image->gainMap->baseHdrHeadroom)) {
        std::cout << "Unable to express " << arg_max_headroom_.value()
                  << " as a fraction";
        return AVIF_RESULT_INVALID_ARGUMENT;
      }
    }
    if (arg_max_headroom_.value() *
            base_image->gainMap->alternateHdrHeadroom.d <
        base_image->gainMap->alternateHdrHeadroom.n) {
      if (!avifDoubleToUnsignedFraction(
              arg_max_headroom_.value(),
              &base_image->gainMap->alternateHdrHeadroom)) {
        std::cout << "Unable to express " << arg_max_headroom_.value()
                  << " as a fraction";
        return AVIF_RESULT_INVALID_ARGUMENT;
      }
    }
  }

  EncoderPtr encoder(avifEncoderCreate());
  if (encoder == nullptr) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  }
  encoder->quality = arg_image_encode_.quality;
  encoder->qualityAlpha = arg_image_encode_.quality_alpha;
  encoder->qualityGainMap = arg_gain_map_quality_;
  encoder->speed = arg_image_encode_.speed;
  result = WriteAvif(base_image.get(), encoder.get(), arg_output_filename_);
  if (result != AVIF_RESULT_OK) {
    std::cout << "Failed to encode image: " << avifResultToString(result)
              << " (" << encoder->diag.error << ")\n";
    return result;
  }

  return AVIF_RESULT_OK;
}

}  // namespace avif
