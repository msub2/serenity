/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/ICC/Profile.h>
#include <LibGfx/ImageFormats/BMPWriter.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibGfx/ImageFormats/JPEGWriter.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/ImageFormats/PortableFormatWriter.h>
#include <LibGfx/ImageFormats/QOIWriter.h>

using AnyBitmap = Variant<RefPtr<Gfx::Bitmap>, RefPtr<Gfx::CMYKBitmap>>;
struct LoadedImage {
    Gfx::NaturalFrameFormat internal_format;
    AnyBitmap bitmap;

    Optional<ReadonlyBytes> icc_data;
};

static ErrorOr<LoadedImage> load_image(RefPtr<Gfx::ImageDecoder> const& decoder, int frame_index)
{
    auto internal_format = decoder->natural_frame_format();

    auto bitmap = TRY([&]() -> ErrorOr<AnyBitmap> {
        switch (internal_format) {
        case Gfx::NaturalFrameFormat::RGB:
        case Gfx::NaturalFrameFormat::Grayscale:
        case Gfx::NaturalFrameFormat::Vector:
            return TRY(decoder->frame(frame_index)).image;
        case Gfx::NaturalFrameFormat::CMYK:
            return RefPtr(TRY(decoder->cmyk_frame()));
        }
        VERIFY_NOT_REACHED();
    }());

    return LoadedImage { internal_format, move(bitmap), TRY(decoder->icc_data()) };
}

static ErrorOr<void> do_move_alpha_to_rgb(LoadedImage& image)
{
    if (!image.bitmap.has<RefPtr<Gfx::Bitmap>>())
        return Error::from_string_view("Can't --move-alpha-to-rgb with CMYK bitmaps"sv);
    auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();

    switch (frame->format()) {
    case Gfx::BitmapFormat::Invalid:
        return Error::from_string_view("Can't --move-alpha-to-rgb with invalid bitmaps"sv);
    case Gfx::BitmapFormat::RGBA8888:
        // No image decoder currently produces bitmaps with this format.
        // If that ever changes, preferrably fix the image decoder to use BGRA8888 instead :)
        // If there's a good reason for not doing that, implement support for this, I suppose.
        return Error::from_string_view("--move-alpha-to-rgb not implemented for RGBA8888"sv);
    case Gfx::BitmapFormat::BGRA8888:
    case Gfx::BitmapFormat::BGRx8888:
        // FIXME: If BitmapFormat::Gray8 existed (and image encoders made use of it to write grayscale images), we could use it here.
        for (auto& pixel : *frame) {
            u8 alpha = pixel >> 24;
            pixel = 0xff000000 | (alpha << 16) | (alpha << 8) | alpha;
        }
    }
    return {};
}

static ErrorOr<void> do_strip_alpha(LoadedImage& image)
{
    if (!image.bitmap.has<RefPtr<Gfx::Bitmap>>())
        return Error::from_string_view("Can't --strip-alpha with CMYK bitmaps"sv);
    auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();

    switch (frame->format()) {
    case Gfx::BitmapFormat::Invalid:
        return Error::from_string_view("Can't --strip-alpha with invalid bitmaps"sv);
    case Gfx::BitmapFormat::RGBA8888:
        // No image decoder currently produces bitmaps with this format.
        // If that ever changes, preferrably fix the image decoder to use BGRA8888 instead :)
        // If there's a good reason for not doing that, implement support for this, I suppose.
        return Error::from_string_view("--strip-alpha not implemented for RGBA8888"sv);
    case Gfx::BitmapFormat::BGRA8888:
    case Gfx::BitmapFormat::BGRx8888:
        frame->strip_alpha_channel();
    }
    return {};
}

static ErrorOr<OwnPtr<Core::MappedFile>> convert_image_profile(LoadedImage& image, StringView convert_color_profile_path, OwnPtr<Core::MappedFile> maybe_source_icc_file)
{
    if (!image.icc_data.has_value())
        return Error::from_string_view("No source color space embedded in image. Pass one with --assign-color-profile."sv);

    auto source_icc_file = move(maybe_source_icc_file);
    auto source_icc_data = image.icc_data.value();
    auto icc_file = TRY(Core::MappedFile::map(convert_color_profile_path));
    image.icc_data = icc_file->bytes();

    auto source_profile = TRY(Gfx::ICC::Profile::try_load_from_externally_owned_memory(source_icc_data));
    auto destination_profile = TRY(Gfx::ICC::Profile::try_load_from_externally_owned_memory(icc_file->bytes()));

    if (destination_profile->data_color_space() != Gfx::ICC::ColorSpace::RGB)
        return Error::from_string_view("Can only convert to RGB at the moment, but destination color space is not RGB"sv);

    if (image.bitmap.has<RefPtr<Gfx::CMYKBitmap>>()) {
        if (source_profile->data_color_space() != Gfx::ICC::ColorSpace::CMYK)
            return Error::from_string_view("Source image data is CMYK but source color space is not CMYK"sv);

        auto& cmyk_frame = image.bitmap.get<RefPtr<Gfx::CMYKBitmap>>();
        auto rgb_frame = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, cmyk_frame->size()));
        TRY(destination_profile->convert_cmyk_image(*rgb_frame, *cmyk_frame, *source_profile));
        image.bitmap = RefPtr(move(rgb_frame));
        image.internal_format = Gfx::NaturalFrameFormat::RGB;
    } else {
        // FIXME: This likely wrong for grayscale images because they've been converted to
        //        RGB at this point, but their embedded color profile is still for grayscale.
        auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();
        TRY(destination_profile->convert_image(*frame, *source_profile));
    }

    return icc_file;
}

static ErrorOr<void> save_image(LoadedImage& image, StringView out_path, bool ppm_ascii, u8 jpeg_quality)
{
    if (!image.bitmap.has<RefPtr<Gfx::Bitmap>>())
        return Error::from_string_view("Can't save CMYK bitmaps yet, convert to RGB first with --convert-to-color-profile"sv);
    auto& frame = image.bitmap.get<RefPtr<Gfx::Bitmap>>();

    auto output_stream = TRY(Core::File::open(out_path, Core::File::OpenMode::Write));
    auto buffered_stream = TRY(Core::OutputBufferedFile::create(move(output_stream)));

    if (out_path.ends_with(".jpg"sv, CaseSensitivity::CaseInsensitive) || out_path.ends_with(".jpeg"sv, CaseSensitivity::CaseInsensitive)) {
        TRY(Gfx::JPEGWriter::encode(*buffered_stream, *frame, { .icc_data = image.icc_data, .quality = jpeg_quality }));
        return {};
    }
    if (out_path.ends_with(".ppm"sv, CaseSensitivity::CaseInsensitive)) {
        auto const format = ppm_ascii ? Gfx::PortableFormatWriter::Options::Format::ASCII : Gfx::PortableFormatWriter::Options::Format::Raw;
        TRY(Gfx::PortableFormatWriter::encode(*buffered_stream, *frame, { .format = format }));
        return {};
    }

    ByteBuffer bytes;
    if (out_path.ends_with(".bmp"sv, CaseSensitivity::CaseInsensitive)) {
        bytes = TRY(Gfx::BMPWriter::encode(*frame, { .icc_data = image.icc_data }));
    } else if (out_path.ends_with(".png"sv, CaseSensitivity::CaseInsensitive)) {
        bytes = TRY(Gfx::PNGWriter::encode(*frame, { .icc_data = image.icc_data }));
    } else if (out_path.ends_with(".qoi"sv, CaseSensitivity::CaseInsensitive)) {
        bytes = TRY(Gfx::QOIWriter::encode(*frame));
    } else {
        return Error::from_string_view("can only write .bmp, .jpg, .png, .ppm, and .qoi"sv);
    }
    TRY(buffered_stream->write_until_depleted(bytes));

    return {};
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    Core::ArgsParser args_parser;

    StringView in_path;
    args_parser.add_positional_argument(in_path, "Path to input image file", "FILE");

    StringView out_path;
    args_parser.add_option(out_path, "Path to output image file", "output", 'o', "FILE");

    bool no_output = false;
    args_parser.add_option(no_output, "Do not write output (only useful for benchmarking image decoding)", "no-output", {});

    int frame_index = 0;
    args_parser.add_option(frame_index, "Which frame of a multi-frame input image (0-based)", "frame-index", {}, "INDEX");

    bool move_alpha_to_rgb = false;
    args_parser.add_option(move_alpha_to_rgb, "Copy alpha channel to rgb, clear alpha", "move-alpha-to-rgb", {});

    bool ppm_ascii = false;
    args_parser.add_option(ppm_ascii, "Convert to a PPM in ASCII", "ppm-ascii", {});

    bool strip_alpha = false;
    args_parser.add_option(strip_alpha, "Remove alpha channel", "strip-alpha", {});

    StringView assign_color_profile_path;
    args_parser.add_option(assign_color_profile_path, "Load color profile from file and assign it to output image", "assign-color-profile", {}, "FILE");

    StringView convert_color_profile_path;
    args_parser.add_option(convert_color_profile_path, "Load color profile from file and convert output image from current profile to loaded profile", "convert-to-color-profile", {}, "FILE");

    bool strip_color_profile = false;
    args_parser.add_option(strip_color_profile, "Do not write color profile to output", "strip-color-profile", {});

    u8 quality = 75;
    args_parser.add_option(quality, "Quality used for the JPEG encoder, the default value is 75 on a scale from 0 to 100", "quality", {}, {});

    args_parser.parse(arguments);

    if (out_path.is_empty() ^ no_output)
        return Error::from_string_view("exactly one of -o or --no-output is required"sv);

    auto file = TRY(Core::MappedFile::map(in_path));
    auto decoder = Gfx::ImageDecoder::try_create_for_raw_bytes(file->bytes());
    if (!decoder)
        return Error::from_string_view("Failed to decode input file"sv);

    LoadedImage image = TRY(load_image(*decoder, frame_index));

    if (move_alpha_to_rgb)
        TRY(do_move_alpha_to_rgb(image));

    if (strip_alpha)
        TRY(do_strip_alpha(image));

    OwnPtr<Core::MappedFile> icc_file;
    if (!assign_color_profile_path.is_empty()) {
        icc_file = TRY(Core::MappedFile::map(assign_color_profile_path));
        image.icc_data = icc_file->bytes();
    }

    if (!convert_color_profile_path.is_empty())
        icc_file = TRY(convert_image_profile(image, convert_color_profile_path, move(icc_file)));

    if (strip_color_profile)
        image.icc_data.clear();

    if (no_output)
        return 0;

    TRY(save_image(image, out_path, ppm_ascii, quality));

    return 0;
}
