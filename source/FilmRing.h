#pragma once

#include <FFGLSDK.h>

#include <cstddef>

namespace handleman
{
/**
	The loop of film between the camera and the projector: an RGBA8 texture
	array, one layer per exposed picture, and a framebuffer to expose into
	any layer of it.

	**It grows; it never clears what it holds.** The depth it needs depends
	on the two hands (how far the camera can run ahead of the projector), so
	Ensure() reallocates when a deeper ring or a new raster is asked for --
	and a reallocated texture is a cleared one, while the pictures in it are
	still to be projected. So every layer is copied (same raster: exactly;
	new raster: bilinear) into the new texture before the old one is
	deleted. It never shrinks while it lives: a hand turned down again keeps
	the memory it was given until DeInitGL.

	RGBA8 holds a host's 8-bit picture exactly (the code values, decoded in
	the output pass); a 16-bit composition is quantised to 8 bits here.
*/
class FilmRing
{
public:
	~FilmRing();

	/// At least `depth` layers at width x height. `keep` false is the
	/// negative control: the old pictures are not carried across.
	bool Ensure( int width, int height, int depth, ffglex::FFGLShader& resample, ffglex::FFGLScreenQuad& quad, bool keep );

	/// Bind this ring's framebuffer with `layer` attached and the viewport
	/// set to the ring's raster. The caller binds back whatever it needs.
	void BindLayer( int layer );

	void Destroy();

	bool IsValid() const { return texture != 0; }
	GLuint Texture() const { return texture; }
	int Width() const { return width; }
	int Height() const { return height; }
	int Depth() const { return depth; }
	size_t Bytes() const { return static_cast< size_t >( width ) * height * depth * 4; }

private:
	GLuint allocate( int w, int h, int d );

	GLuint texture = 0;
	GLuint fbo     = 0;
	int width = 0, height = 0, depth = 0;
};

} // namespace handleman
