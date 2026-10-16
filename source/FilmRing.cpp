#include "FilmRing.h"

#include <algorithm>

namespace handleman
{
FilmRing::~FilmRing()
{
	//A destructor cannot know a GL context is current; DeInitGL calls
	//Destroy() while one is. Anything left here is the host tearing down.
}

GLuint FilmRing::allocate( int w, int h, int d )
{
	GLuint t = 0;
	glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D_ARRAY, t );
	glTexImage3D( GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0 );
	glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
	return t;
}

bool FilmRing::Ensure( int w, int h, int d, ffglex::FFGLShader& resample, ffglex::FFGLScreenQuad& quad, bool keep )
{
	if( texture != 0 && w == width && h == height && d <= depth )
		return true;
	if( w <= 0 || h <= 0 || d <= 0 )
		return false;

	const int newDepth = std::max( d, depth );
	const GLuint fresh = allocate( w, h, newDepth );
	if( fresh == 0 )
		return false;
	if( fbo == 0 )
		glGenFramebuffers( 1, &fbo );

	GLint previousFbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFbo );
	GLint previousViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, previousViewport );

	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, w, h );
	bool complete = true;
	for( int layer = 0; layer < newDepth; ++layer )
	{
		glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, fresh, 0, layer );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			complete = false;
			break;
		}
		//A new texture's contents are whatever the driver handed back.
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( keep && texture != 0 && layer < depth )
		{
			glUseProgram( resample.GetGLID() );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D_ARRAY, texture );
			resample.Set( "Old", 0 );
			resample.Set( "Layer", layer );
			quad.Draw();
			glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
			glUseProgram( 0 );
		}
	}
	glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFbo ) );
	glViewport( previousViewport[ 0 ], previousViewport[ 1 ], previousViewport[ 2 ], previousViewport[ 3 ] );

	if( !complete )
	{
		glDeleteTextures( 1, &fresh );
		return false;
	}
	if( texture != 0 )
		glDeleteTextures( 1, &texture );
	texture = fresh;
	width   = w;
	height  = h;
	depth   = newDepth;
	return true;
}

void FilmRing::BindLayer( int layer )
{
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTextureLayer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, layer );
	glViewport( 0, 0, width, height );
}

void FilmRing::Destroy()
{
	if( fbo != 0 )
		glDeleteFramebuffers( 1, &fbo );
	if( texture != 0 )
		glDeleteTextures( 1, &texture );
	fbo = texture = 0;
	width = height = depth = 0;
}

} // namespace handleman
