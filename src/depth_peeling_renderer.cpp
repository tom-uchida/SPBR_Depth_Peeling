/*****************************************************************************/
/**
 *  @file   depth_peeling_renderer.cpp
 *  @author Tomomasa Uchida
 */
/*****************************************************************************/
#include "depth_peeling_renderer.h"

#include <kvs/OpenGL>
#include <kvs/ShaderSource>
#include <kvs/VertexShader>
#include <kvs/FragmentShader>
#include "GLdef.h"


namespace
{

/*===========================================================================*/
/**
 *  @brief  Returns vertex-color array.
 *  @param  point [in] pointer to the point object
 */
/*===========================================================================*/
kvs::ValueArray<kvs::UInt8> VertexColors( const kvs::PointObject* point_object )
{
    const size_t nvertices = point_object->numberOfVertices();
    const bool is_single_color = point_object->colors().size() == 3;
    const kvs::UInt8* pcolors = point_object->colors().data();

    kvs::ValueArray<kvs::UInt8> colors( nvertices * 4 );
    for ( size_t i = 0; i < nvertices; i++ )
    {
        colors[ 4 * i + 0 ] = is_single_color ? pcolors[0] : pcolors[ 3 * i + 0 ];
        colors[ 4 * i + 1 ] = is_single_color ? pcolors[1] : pcolors[ 3 * i + 1 ];
        colors[ 4 * i + 2 ] = is_single_color ? pcolors[2] : pcolors[ 3 * i + 2 ];
        colors[ 4 * i + 3 ] = 255; // point_object has no opacity
    }

    return colors;
}

/*===========================================================================*/
/**
 *  @brief  Returns vertex-normal array.
 *  @param  point [in] pointer to the point object
 */
/*===========================================================================*/
kvs::ValueArray<kvs::Real32> VertexNormals( const kvs::PointObject* point_object )
{
    return kvs::ValueArray<kvs::Real32>();
}

void DrawRect()
{
    kvs::OpenGL::WithPushedMatrix p1( GL_MODELVIEW );
    p1.loadIdentity();
    {
        kvs::OpenGL::WithPushedMatrix p2( GL_PROJECTION );
        p2.loadIdentity();
        {
            kvs::OpenGL::SetOrtho( 0, 1, 0, 1, -1, 1 );
            kvs::OpenGL::Begin( GL_QUADS );
            kvs::OpenGL::TexCoordVertex( kvs::Vec2( 1, 1 ), kvs::Vec2( 1, 1 ) );
            kvs::OpenGL::TexCoordVertex( kvs::Vec2( 0, 1 ), kvs::Vec2( 0, 1 ) );
            kvs::OpenGL::TexCoordVertex( kvs::Vec2( 0, 0 ), kvs::Vec2( 0, 0 ) );
            kvs::OpenGL::TexCoordVertex( kvs::Vec2( 1, 0 ), kvs::Vec2( 1, 0 ) );
            kvs::OpenGL::End();
        }
    }
}

} // end of namespace


namespace local
{

DepthPeelingRenderer::DepthPeelingRenderer():
    m_width( 0 ),
    m_height( 0 ),
    m_object( NULL ),
    m_shader( NULL ),
    m_layer_level( 1 ),
    m_background_color( kvs::RGBColor( 0, 0, 0 ) )
{
    // kvs::glsl::ParticleBasedRenderer : public kvs::StochasticRendererBase
    // kvs::StochasticRendererBase : public kvs::RendererBase
    this->setShader( kvs::Shader::Lambert() );
}

DepthPeelingRenderer::~DepthPeelingRenderer()
{
    if ( m_shader ) { delete m_shader; }
}

// Scene.cpp:
//  687: void Scene::paintFunction()
//  709: renderer->exec( object, m_camera, m_light );
void DepthPeelingRenderer::exec( kvs::ObjectBase* _object, kvs::Camera* _camera, kvs::Light* _light )
{
    // DownCast: kvs::ObjectBase* → kvs::PointObject*
    kvs::PointObject* point_object = kvs::PointObject::DownCast( _object );
    setEnabledShading( false );

    BaseClass::startTimer();
    kvs::OpenGL::WithPushedAttrib p( GL_ALL_ATTRIB_BITS );

    // Prepare for rendering
    const size_t width = _camera->windowWidth();
    const size_t height = _camera->windowHeight();
    const bool window_created = m_width == 0 && m_height == 0;
    if ( window_created )
    {
        m_width = width;
        m_height = height;
        m_object = _object;
        this->create_shader_program();
        this->create_vbo( point_object );
        this->create_framebuffer( width, height );
    }

    const bool window_resized = m_width != width || m_height != height;
    if ( window_resized )
    {
        m_width = width;
        m_height = height;
        this->update_framebuffer( width, height );
    }

    const bool object_changed = m_object != _object;
    if ( object_changed )
    {
        m_object = _object;
        m_vbo.release();
        m_ibo.release();
        m_peeling_shader.release();
        m_blending_shader.release();
        m_finalizing_shader.release();
        this->create_shader_program();
        this->create_vbo( point_object );
    }

    // Exec. peeling processing 
    this->initialize_pass();
    for ( size_t i = 0; i < m_layer_level; i++ )
    {
        this->peel_pass( point_object );
    }
    this->finalize_pass();

    BaseClass::stopTimer();
} // End of exec()

void DepthPeelingRenderer::create_shader_program()
{
    // Build depth peeling shader
    {
        kvs::ShaderSource vert( "shaders/peeling.vert" );
        kvs::ShaderSource frag( "shaders/peeling.frag" );
        if ( isEnabledShading() )
        {
            switch ( m_shader->type() )
            {
            case kvs::Shader::LambertShading: frag.define("ENABLE_LAMBERT_SHADING"); break;
            case kvs::Shader::PhongShading: frag.define("ENABLE_PHONG_SHADING"); break;
            case kvs::Shader::BlinnPhongShading: frag.define("ENABLE_BLINN_PHONG_SHADING"); break;
            default: break; // NO SHADING
            }

            if ( kvs::OpenGL::Boolean( GL_LIGHT_MODEL_TWO_SIDE ) == GL_TRUE )
            {
                frag.define("ENABLE_TWO_SIDE_LIGHTING");
            }
        }

        m_peeling_shader.build( vert, frag );
        m_peeling_shader.bind();
        m_peeling_shader.setUniform( "shading.Ka", m_shader->Ka );
        m_peeling_shader.setUniform( "shading.Kd", m_shader->Kd );
        m_peeling_shader.setUniform( "shading.Ks", m_shader->Ks );
        m_peeling_shader.setUniform( "shading.S",  m_shader->S );
        m_peeling_shader.setUniform( "depth_front", 10 );
        m_peeling_shader.unbind();
    }

    // Build blending shader
    {
        kvs::ShaderSource vert( "shaders/blending.vert" );
        kvs::ShaderSource frag( "shaders/blending.frag" );

        m_blending_shader.build( vert, frag );
        m_blending_shader.bind();
        m_blending_shader.setUniform( "color_front", 11 );
        m_blending_shader.setUniform( "depth_back", 12 );
        m_blending_shader.setUniform( "color_back", 13 );
        m_blending_shader.unbind();
    }

    // Build finalizing shader
    {
        kvs::ShaderSource vert( "shaders/finalizing.vert" );
        kvs::ShaderSource frag( "shaders/finalizing.frag" );

        m_finalizing_shader.build( vert, frag );
        m_finalizing_shader.bind();
        m_finalizing_shader.setUniform( "color_buffer", 0 );
        m_finalizing_shader.setUniform( "background_color", m_background_color.toVec3() );
        m_finalizing_shader.unbind();
    }
} // End of create_shader_program()

void DepthPeelingRenderer::create_vbo( const kvs::PointObject* _point_object )
{
    kvs::ValueArray<kvs::Real32> coords  = _point_object->coords();
    kvs::ValueArray<kvs::UInt8>  colors  = ::VertexColors( _point_object );
    kvs::ValueArray<kvs::Real32> normals = ::VertexNormals( _point_object );

    const size_t coord_size  = coords.byteSize();
    const size_t color_size  = colors.byteSize();
    const size_t normal_size = normals.byteSize();
    const size_t byte_size   = coord_size + color_size + normal_size;

    m_vbo.create( byte_size );
    m_vbo.bind();
    m_vbo.load( coord_size, coords.data(), 0 );
    m_vbo.load( color_size, colors.data(), coord_size );
    if ( normal_size > 0 )
    {
        m_vbo.load( normal_size, normals.data(), coord_size + color_size );
    }
    m_vbo.unbind();
} // End of create_buffer_object()

void DepthPeelingRenderer::create_framebuffer( const size_t _width, const size_t _height )
{
    for ( size_t i = 0; i < 3; i++ )
    {
        m_color_buffer[i].setWrapS( GL_REPEAT );
        m_color_buffer[i].setWrapT( GL_REPEAT );
        m_color_buffer[i].setMinFilter( GL_NEAREST );
        m_color_buffer[i].setMagFilter( GL_NEAREST );
        m_color_buffer[i].setPixelFormat( GL_RGBA32F, GL_RGBA, GL_UNSIGNED_BYTE );
        m_color_buffer[i].create( _width, _height );

        m_depth_buffer[i].setWrapS( GL_REPEAT );
        m_depth_buffer[i].setWrapT( GL_REPEAT );
        m_depth_buffer[i].setMinFilter( GL_NEAREST );
        m_depth_buffer[i].setMagFilter( GL_NEAREST );
        m_depth_buffer[i].setPixelFormat( GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE );
        m_depth_buffer[i].create( _width, _height );

        m_framebuffer[i].create();
        m_framebuffer[i].attachColorTexture( m_color_buffer[i] );
        m_framebuffer[i].attachDepthTexture( m_depth_buffer[i] );
    }

    m_peeling_shader.bind();
    m_peeling_shader.setUniform( "width", static_cast<GLfloat>( _width ) );
    m_peeling_shader.setUniform( "height", static_cast<GLfloat>( _height ) );
    m_peeling_shader.unbind();

    m_blending_shader.bind();
    m_blending_shader.setUniform( "width", static_cast<GLfloat>( _width ) );
    m_blending_shader.setUniform( "height", static_cast<GLfloat>( _height ) );
    m_blending_shader.unbind();

    m_finalizing_shader.bind();
    m_finalizing_shader.setUniform( "width", static_cast<GLfloat>( _width ) );
    m_finalizing_shader.setUniform( "height", static_cast<GLfloat>( _height ) );
    m_finalizing_shader.unbind();
} // End of create_framebuffer()

void DepthPeelingRenderer::update_framebuffer( const size_t _width, const size_t _height )
{
    for ( size_t i = 0; i < 3; i++ )
    {
        m_color_buffer[i].release();
        m_color_buffer[i].create( _width, _height );

        m_depth_buffer[i].release();
        m_depth_buffer[i].create( _width, _height );

        m_framebuffer[i].attachColorTexture( m_color_buffer[i] );
        m_framebuffer[i].attachDepthTexture( m_depth_buffer[i] );
    }

    m_peeling_shader.bind();
    m_peeling_shader.setUniform( "width", static_cast<GLfloat>( _width ) );
    m_peeling_shader.setUniform( "height", static_cast<GLfloat>( _height ) );
    m_peeling_shader.unbind();

    m_blending_shader.bind();
    m_blending_shader.setUniform( "width", static_cast<GLfloat>( _width ) );
    m_blending_shader.setUniform( "height", static_cast<GLfloat>( _height ) );
    m_blending_shader.unbind();

    m_finalizing_shader.bind();
    m_finalizing_shader.setUniform( "width", static_cast<GLfloat>( _width ) );
    m_finalizing_shader.setUniform( "height", static_cast<GLfloat>( _height ) );
    m_finalizing_shader.unbind();
} // End of update_framebuffer()

void DepthPeelingRenderer::initialize_pass()
{
    m_cycle = 0;
    kvs::FrameBufferObject::Binder fbo( m_framebuffer[0] );
    kvs::OpenGL::SetDrawBuffer( GL_COLOR_ATTACHMENT0 );
    kvs::OpenGL::SetClearColor( kvs::Vec4::Zero() );
    kvs::OpenGL::SetClearDepth( 0.0 );
    kvs::OpenGL::Clear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

void DepthPeelingRenderer::finalize_pass()
{
    kvs::Texture::Binder tex0( m_color_buffer[2], 0 );
    // kvs::Texture::Binder tex0( m_depth_buffer[2], 0 );
    kvs::ProgramObject::Binder shader( m_finalizing_shader );

    kvs::OpenGL::Disable( GL_DEPTH_TEST );
    ::DrawRect();
}

void DepthPeelingRenderer::peel_pass( const kvs::PointObject* _point_object )
{
    const int front = m_cycle; // 0 or 1
    const int back = 2;
    const int target = ( m_cycle + 1 ) % 2; // 1 or 0
    m_cycle = target;

    kvs::FrameBufferObject::Binder fbo4back( m_framebuffer[back] );
    {
        kvs::OpenGL::SetDrawBuffer( GL_COLOR_ATTACHMENT0 );
        kvs::OpenGL::SetClearColor( kvs::Vec4::Zero() );
        kvs::OpenGL::SetClearDepth( 1.0 );
        kvs::OpenGL::Clear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

        // Assign texture to the fbo
        kvs::Texture::Binder tex10( m_depth_buffer[front], 10 );

        // Rendering to the fbo (Offscreen Rendering)
        this->draw( _point_object );
    }

    kvs::FrameBufferObject::Binder fbo4target( m_framebuffer[target] );
    {
        kvs::OpenGL::SetDrawBuffer( GL_COLOR_ATTACHMENT0 );
        kvs::OpenGL::SetClearColor( kvs::Vec4::Zero() );
        kvs::OpenGL::SetClearDepth( 1.0 );
        kvs::OpenGL::Clear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

        // Assign texture to the fbo
        kvs::Texture::Binder tex11( m_color_buffer[front], 11 );
        kvs::Texture::Binder tex12( m_depth_buffer[back],  12 );
        kvs::Texture::Binder tex13( m_color_buffer[back],  13 );

        // Rendering to the fbo ( Offscreen Rendering? )
        this->blend();
    }

} // End of peel_pass()

void DepthPeelingRenderer::draw( const kvs::PointObject* _point_object )
{
    kvs::VertexBufferObject::Binder vbo( m_vbo );
    kvs::ProgramObject::Binder shader( m_peeling_shader );

    kvs::OpenGL::Enable( GL_DEPTH_TEST );

    const kvs::Mat4  M = kvs::OpenGL::ModelViewMatrix();
    const kvs::Mat4 PM = kvs::OpenGL::ProjectionMatrix() * M;
    const kvs::Mat3  N = kvs::Mat3( M[0].xyz(), M[1].xyz(), M[2].xyz() );
    m_peeling_shader.setUniform( "ModelViewMatrix", M );
    m_peeling_shader.setUniform( "ModelViewProjectionMatrix", PM );
    m_peeling_shader.setUniform( "NormalMatrix", N );

    const size_t npoints    = _point_object->numberOfVertices();
    const size_t coord_size = npoints * 3 * sizeof( kvs::Real32 );
    // const size_t color_size = npoints * 4 * sizeof( kvs::UInt8 );

    // Enable coords.
    KVS_GL_CALL( glEnableClientState( GL_VERTEX_ARRAY ) );
    KVS_GL_CALL( glVertexPointer( 3, GL_FLOAT, 0, (GLbyte*)NULL + 0 ) );

    // Enable colors.
    KVS_GL_CALL( glEnableClientState( GL_COLOR_ARRAY ) );
    KVS_GL_CALL( glColorPointer( 4, GL_UNSIGNED_BYTE, 0, (GLbyte*)NULL + coord_size ) );

    // Draw points.
    KVS_GL_CALL( glDrawArrays( GL_POINTS, 0, npoints ) );

    // Disable coords.
    KVS_GL_CALL( glDisableClientState( GL_VERTEX_ARRAY ) );

    // Disable colors.
    KVS_GL_CALL( glDisableClientState( GL_COLOR_ARRAY ) );
}

void DepthPeelingRenderer::blend()
{
    kvs::ProgramObject::Binder bind( m_blending_shader );
    kvs::OpenGL::Enable( GL_DEPTH_TEST );

    kvs::OpenGL::SetDepthFunc( GL_ALWAYS );
    ::DrawRect();
    // Obtain depth buffer by executing point occlusion effect.
    // GL_LESS: Regarding the target shape, priority is given to the one that is near from one direction.
    kvs::OpenGL::SetDepthFunc( GL_LESS );
}

} // end of namespace local
