/*
 * Copyright (c) 2012-2017 Daniele Bartolini and individual contributors.
 * License: https://github.com/taylor001/crown/blob/master/LICENSE
 */

#pragma once

#include "core/filesystem/types.h"
#include "core/memory/types.h"
#include "core/types.h"
#include "resource/types.h"

namespace crown
{
struct FontResource
{
	u32 version;
	u32 num_glyphs;
	u32 texture_size;
	u32 font_size;
};

struct GlyphData
{
	f32 x;
	f32 y;
	f32 width;
	f32 height;
	f32 x_offset;
	f32 y_offset;
	f32 x_advance;
};

typedef u32 CodePoint;

namespace font_resource_internal
{
	void compile(const char* path, CompileOptions& opts);

} // namespace font_resource_internal

namespace font_resource
{
	/// Returns the glyph for the code point @a cp.
	const GlyphData* get_glyph(const FontResource* fr, CodePoint cp);

} // namespace font_resource

} // namespace crown
