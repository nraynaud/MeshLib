#include "MRRenderMeshObject.h"
#include "MRMesh/MRObjectMesh.h"
#include "MRMesh/MRTimer.h"
#include "MRCreateShader.h"
#include "MRMesh/MRMesh.h"
#include "MRMesh/MRBitSet.h"
#include "MRMesh/MRPlane3.h"
#include "MRGLMacro.h"
#include "MRMesh/MRBitSetParallelFor.h"
#include "MRShadersHolder.h"
#include "MRRenderGLHelpers.h"
#include "MRRenderHelpers.h"
#include "MRMeshViewer.h"
#include "MRGladGlfw.h"

#define DIRTY_EDGE 0x40000
static_assert( DIRTY_EDGE == MR::DIRTY_ALL + 1 );

namespace
{
constexpr std::size_t highestBit( uint32_t v )
{
    std::size_t i;
    for ( i = 0; v != 1; i++ )
        v >>= 1;
    return i;
}
}

namespace MR
{

template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_POSITION> { using type = Vector3f; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_VERTS_RENDER_NORMAL> { using type = Vector3f; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_FACES_RENDER_NORMAL> { using type = Vector4f; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_VERTS_COLORMAP> { using type = Color; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_FACE> { using type = Vector3i; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_EDGE> { using type = Vector2i; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_UV> { using type = UVCoord; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_SELECTION> { using type = unsigned; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_BORDER_LINES> { using type = Vector3f; };
template<> struct RenderMeshObject::BufferTypeHelper<DIRTY_EDGES_SELECTION> { using type = Vector3f; };

template <typename T>
class RenderMeshObject::BufferRef
{
    T* data_;
    std::size_t glSize_;
    DirtyFlag* dirtyMask_;
    DirtyFlag dirtyFlag_;

public:
    BufferRef( T* data, std::size_t glSize, DirtyFlag* dirtyMask, DirtyFlag dirtyFlag ) noexcept
        : data_( data )
        , glSize_( glSize )
        , dirtyMask_( dirtyMask )
        , dirtyFlag_( dirtyFlag )
    {
        assert( !dirtyMask_ || ( *dirtyMask_ & dirtyFlag_ ) );
    }
    BufferRef( BufferRef<T>&& other ) noexcept
        : data_( other.data_ )
        , glSize_( other.glSize_ )
        , dirtyMask_( other.dirtyMask_ )
        , dirtyFlag_( other.dirtyFlag_ )
    {
        other.dirtyMask_ = nullptr;
    }
    BufferRef( const BufferRef<T>& ) = delete;
    ~BufferRef()
    {
        if ( dirtyMask_ )
            *dirtyMask_ &= ~dirtyFlag_;
    }

    T& operator []( std::size_t i ) const noexcept { return data_[i]; }
    T* data() const noexcept { return data_; };
    /// returns actual buffer size
    [[nodiscard]] std::size_t size() const noexcept { return data_ ? glSize_ : 0; }
    /// returns number of elements that are about to be loaded or already loaded to GL memory
    [[nodiscard]] std::size_t glSize() const noexcept { return glSize_; }
    [[nodiscard]] bool dirty() const noexcept { return dirtyMask_ && ( *dirtyMask_ & dirtyFlag_ ); }
};

RenderMeshObject::RenderMeshObject( const VisualObject& visObj )
{
    objMesh_ = dynamic_cast< const ObjectMeshHolder* >( &visObj );
    assert( objMesh_ );
    if ( Viewer::constInstance()->isGLInitialized() )
        initBuffers_();
    bufferMode_ = MemoryEfficient;
}

RenderMeshObject::~RenderMeshObject()
{
    freeBuffers_();
}

void RenderMeshObject::render( const RenderParams& renderParams )
{
    if ( !objMesh_->mesh() )
        return;
    if ( !Viewer::constInstance()->isGLInitialized() )
    {
        objMesh_->resetDirty();
        return;
    }
    update_( renderParams.viewportId );

    if ( renderParams.alphaSort )
    {
        GL_EXEC( glDepthMask( GL_FALSE ) );
        GL_EXEC( glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE ) );
        GL_EXEC( glDisable( GL_MULTISAMPLE ) );
    }
    else
    {
        GL_EXEC( glDepthMask( GL_TRUE ) );
        GL_EXEC( glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE ) );
#ifndef __EMSCRIPTEN__
        GL_EXEC( glEnable( GL_MULTISAMPLE ) );
#endif
    }

    // Initialize uniform
    GL_EXEC( glViewport( ( GLsizei )renderParams.viewport.x, ( GLsizei )renderParams.viewport.y,
        ( GLsizei )renderParams.viewport.z, ( GLsizei )renderParams.viewport.w ) );

    if ( objMesh_->getVisualizeProperty( VisualizeMaskType::DepthTest, renderParams.viewportId ) )
    {
        GL_EXEC( glEnable( GL_DEPTH_TEST ) );
    }
    else
    {
        GL_EXEC( glDisable( GL_DEPTH_TEST ) );
    }

    GL_EXEC( glEnable( GL_BLEND ) );
    GL_EXEC( glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA ) );
    bindMesh_( renderParams.alphaSort );

    auto shader = renderParams.alphaSort ? ShadersHolder::getShaderId( ShadersHolder::TransparentMesh ) : ShadersHolder::getShaderId( ShadersHolder::DrawMesh );
    // Send transformations to the GPU
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "model" ), 1, GL_TRUE, renderParams.modelMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "view" ), 1, GL_TRUE, renderParams.viewMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "proj" ), 1, GL_TRUE, renderParams.projMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "normal_matrix" ), 1, GL_TRUE, renderParams.normMatrixPtr ) );

    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "onlyOddFragments" ), objMesh_->getVisualizeProperty( MeshVisualizePropertyType::OnlyOddFragments, renderParams.viewportId ) ) );
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "invertNormals" ), objMesh_->getVisualizeProperty( VisualizeMaskType::InvertedNormals, renderParams.viewportId ) ) );
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "flatShading" ), objMesh_->getVisualizeProperty( MeshVisualizePropertyType::FlatShading, renderParams.viewportId ) ) );
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "perVertColoring" ), objMesh_->getColoringType() == ColoringType::VertsColorMap ) );
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "perFaceColoring" ), objMesh_->getColoringType() == ColoringType::FacesColorMap ) );

    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "useClippingPlane" ), objMesh_->getVisualizeProperty( VisualizeMaskType::ClippedByPlane, renderParams.viewportId ) ) );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "clippingPlane" ),
        renderParams.clipPlane.n.x, renderParams.clipPlane.n.y,
        renderParams.clipPlane.n.z, renderParams.clipPlane.d ) );

    GL_EXEC( auto fixed_colori = glGetUniformLocation( shader, "fixed_color" ) );
    GL_EXEC( glUniform1f( glGetUniformLocation( shader, "specular_exponent" ), objMesh_->getShininess() ) );
    GL_EXEC( glUniform3fv( glGetUniformLocation( shader, "light_position_eye" ), 1, &renderParams.lightPos.x ) );
    GL_EXEC( glUniform4f( fixed_colori, 0.0, 0.0, 0.0, 0.0 ) );

    const auto mainColor = Vector4f( objMesh_->getFrontColor( objMesh_->isSelected() ) );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "mainColor" ), mainColor[0], mainColor[1], mainColor[2], mainColor[3] ) );
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "showSelectedFaces" ), objMesh_->getVisualizeProperty( MeshVisualizePropertyType::SelectedFaces, renderParams.viewportId ) ) );
    const auto selectionColor = Vector4f( objMesh_->getSelectedFacesColor() );
    const auto backColor = Vector4f( objMesh_->getBackColor() );
    const auto selectionBackfacesColor = Vector4f( backColor.x * selectionColor.x, backColor.y * selectionColor.y, backColor.z * selectionColor.z, backColor.w * selectionColor.w );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "selectionColor" ), selectionColor[0], selectionColor[1], selectionColor[2], selectionColor[3] ) );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "selectionBackColor" ), selectionBackfacesColor[0], selectionBackfacesColor[1], selectionBackfacesColor[2], selectionBackfacesColor[3] ) );

    // Render fill
    if ( objMesh_->getVisualizeProperty( MeshVisualizePropertyType::Faces, renderParams.viewportId ) )
    {
        GL_EXEC( glUniform4f( glGetUniformLocation( shader, "backColor" ), backColor[0], backColor[1], backColor[2], backColor[3] ) );

        // Texture
        GL_EXEC( auto useTexture = glGetUniformLocation( shader, "useTexture" ) );
        GL_EXEC( glUniform1i( useTexture, objMesh_->getVisualizeProperty( VisualizeMaskType::Texture, renderParams.viewportId ) ) );

        if ( renderParams.forceZBuffer )
        {
            GL_EXEC( glDepthFunc( GL_ALWAYS ) );
        }
        else
        {
            GL_EXEC( glDepthFunc( GL_LESS ) );
        }

        drawMesh_( true, renderParams.viewportId );
    }
    // Render wireframe
    if ( objMesh_->getVisualizeProperty( MeshVisualizePropertyType::Edges, renderParams.viewportId ) )
        renderMeshEdges_( renderParams );
    if ( objMesh_->getVisualizeProperty( MeshVisualizePropertyType::BordersHighlight, renderParams.viewportId ) )
        renderEdges_<DIRTY_BORDER_LINES>( renderParams, borderArrayObjId_, borderBufferObjId_, objMesh_->getBordersColor() );
    if ( objMesh_->getVisualizeProperty( MeshVisualizePropertyType::SelectedEdges, renderParams.viewportId ) )
        renderEdges_<DIRTY_EDGES_SELECTION>( renderParams, selectedEdgesArrayObjId_, selectedEdgesBufferObjId_, objMesh_->getSelectedEdgesColor() );

    if ( renderParams.alphaSort )
    {
        // enable back masks, disabled for alpha sort
        GL_EXEC( glDepthMask( GL_TRUE ) );
        GL_EXEC( glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE ) );
        GL_EXEC( glEnable( GL_MULTISAMPLE ) );
    }

    if ( bufferMode_ == MemoryEfficient )
        resetBuffers_();
}

void RenderMeshObject::renderPicker( const BaseRenderParams& parameters, unsigned geomId )
{
    if ( !objMesh_->mesh() )
        return;
    if ( !Viewer::constInstance()->isGLInitialized() )
    {
        objMesh_->resetDirty();
        return;
    }
    update_( parameters.viewportId );

    GL_EXEC( glViewport( ( GLsizei )0, ( GLsizei )0, ( GLsizei )parameters.viewport.z, ( GLsizei )parameters.viewport.w ) );

    bindMeshPicker_();

    auto shader = ShadersHolder::getShaderId( ShadersHolder::Picker );

    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "model" ), 1, GL_TRUE, parameters.modelMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "view" ), 1, GL_TRUE, parameters.viewMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "proj" ), 1, GL_TRUE, parameters.projMatrixPtr ) );

    GL_EXEC( glUniform1ui( glGetUniformLocation( shader, "primBucketSize" ), 3 ) );

    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "useClippingPlane" ), objMesh_->getVisualizeProperty( VisualizeMaskType::ClippedByPlane, parameters.viewportId ) ) );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "clippingPlane" ),
        parameters.clipPlane.n.x, parameters.clipPlane.n.y, parameters.clipPlane.n.z, parameters.clipPlane.d ) );
    GL_EXEC( glUniform1ui( glGetUniformLocation( shader, "uniGeomId" ), geomId ) );

    drawMesh_( true, parameters.viewportId, true );

    // do not reset buffers on picker, not to reset buffers that is not used here
    // TODO: rework rendering to have only one buffer and reset it right after it is sent to GPU (need to mix `update_` and `bind_`)
    //if ( bufferMode_ == MemoryEfficient )
    //    resetBuffers_();
}

size_t RenderMeshObject::heapBytes() const
{
    return bufferObj_.heapBytes();
}

template <RenderMeshObject::DirtyFlag dirtyFlag>
void RenderMeshObject::renderEdges_( const RenderParams& renderParams, GLuint vao, GLuint vbo, const Color& colorChar )
{
    auto count = getGLSize_<dirtyFlag>();
    if ( !count )
        return;

    // Send lines data to GL, install lines properties
    GL_EXEC( glBindVertexArray( vao ) );

    auto shader = renderParams.alphaSort ?
        ShadersHolder::getShaderId( ShadersHolder::TransparentMeshBorder ) :
        ShadersHolder::getShaderId( ShadersHolder::MeshBorder );

    GL_EXEC( glUseProgram( shader ) );

    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "view" ), 1, GL_TRUE, renderParams.viewMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "proj" ), 1, GL_TRUE, renderParams.projMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "model" ), 1, GL_TRUE, renderParams.modelMatrixPtr ) );

    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "useClippingPlane" ), objMesh_->getVisualizeProperty( VisualizeMaskType::ClippedByPlane, renderParams.viewportId ) ) );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "clippingPlane" ),
        renderParams.clipPlane.n.x, renderParams.clipPlane.n.y, renderParams.clipPlane.n.z, renderParams.clipPlane.d ) );

    // colors
    auto color = Vector4f( colorChar );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "uniformColor" ),
        color[0], color[1], color[2], color[3] ) );

    // positions
    GL_EXEC( GLint positionId = glGetAttribLocation( shader, "position" ) );
    GL_EXEC( glBindBuffer( GL_ARRAY_BUFFER, vbo ) );
    GL_EXEC( glVertexAttribPointer( positionId, 3, GL_FLOAT, GL_FALSE, 0, 0 ) );
    GL_EXEC( glEnableVertexAttribArray( positionId ) );
    auto buffer = loadBuffer_<dirtyFlag>();
    if ( buffer.dirty() )
    {
        GL_EXEC( glBufferData( GL_ARRAY_BUFFER, sizeof( Vector3f ) * buffer.size(), buffer.data(), GL_DYNAMIC_DRAW ));
    }
    GL_EXEC( glBindVertexArray( vao ) );

    getViewerInstance().incrementThisFrameGLPrimitivesCount( Viewer::GLPrimitivesType::LineArraySize, count / 2 );

    GLfloat width = objMesh_->getEdgeWidth() * 5;
    GL_EXEC( glLineWidth( GLfloat( width ) ) );
    GL_EXEC( glDrawArrays( GL_LINES, 0, int( count ) ) );
}

void RenderMeshObject::renderMeshEdges_( const RenderParams& renderParams )
{
    // Send lines data to GL, install lines properties
    GL_EXEC( glBindVertexArray( meshArrayObjId_ ) );

    auto shader = renderParams.alphaSort ?
        ShadersHolder::getShaderId( ShadersHolder::TransparentMeshBorder ) :
        ShadersHolder::getShaderId( ShadersHolder::MeshBorder );

    GL_EXEC( glUseProgram( shader ) );

    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "view" ), 1, GL_TRUE, renderParams.viewMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "proj" ), 1, GL_TRUE, renderParams.projMatrixPtr ) );
    GL_EXEC( glUniformMatrix4fv( glGetUniformLocation( shader, "model" ), 1, GL_TRUE, renderParams.modelMatrixPtr ) );

    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "useClippingPlane" ), objMesh_->getVisualizeProperty( VisualizeMaskType::ClippedByPlane, renderParams.viewportId ) ) );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "clippingPlane" ),
        renderParams.clipPlane.n.x, renderParams.clipPlane.n.y, renderParams.clipPlane.n.z, renderParams.clipPlane.d ) );

    // colors
    auto color = Vector4f( objMesh_->getEdgesColor() );
    GL_EXEC( glUniform4f( glGetUniformLocation( shader, "uniformColor" ),
        color[0], color[1], color[2], color[3] ) );

    // positions
    auto positions = loadBuffer_<DIRTY_POSITION>();
    bindVertexAttribArray( shader, "position", vertPosBuffer_, positions, 3, positions.dirty(), positions.glSize() != 0 );

    auto edges = loadBuffer_<DIRTY_EDGE>();
    edgesIndicesBuffer_.loadDataOpt( GL_ELEMENT_ARRAY_BUFFER, edges.dirty(), edges );

    getViewerInstance().incrementThisFrameGLPrimitivesCount( Viewer::GLPrimitivesType::LineElementsNum, getGLSize_<DIRTY_EDGE>() );

    GL_EXEC( glLineWidth( GLfloat( objMesh_->getEdgeWidth() ) ) );
    GL_EXEC( glDrawElements( GL_LINES, int( 2 * getGLSize_<DIRTY_EDGE>() ), GL_UNSIGNED_INT, 0 ) );
}

void RenderMeshObject::bindMesh_( bool alphaSort )
{
    auto shader = alphaSort ? ShadersHolder::getShaderId( ShadersHolder::TransparentMesh ) : ShadersHolder::getShaderId( ShadersHolder::DrawMesh );
    GL_EXEC( glBindVertexArray( meshArrayObjId_ ) );
    GL_EXEC( glUseProgram( shader ) );

    auto positions = loadBuffer_<DIRTY_POSITION>();
    bindVertexAttribArray( shader, "position", vertPosBuffer_, positions, 3, positions.dirty(), positions.glSize() != 0 );

    auto normals = loadBuffer_<DIRTY_VERTS_RENDER_NORMAL>();
    bindVertexAttribArray( shader, "normal", vertNormalsBuffer_, normals, 3, normals.dirty(), normals.glSize() != 0 );

    auto colormaps = loadBuffer_<DIRTY_VERTS_COLORMAP>();
    bindVertexAttribArray( shader, "K", vertColorsBuffer_, colormaps, 4, colormaps.dirty(), colormaps.glSize() != 0 );

    auto uvs = loadBuffer_<DIRTY_UV>();
    bindVertexAttribArray( shader, "texcoord", vertUVBuffer_, uvs, 2, uvs.dirty(), uvs.glSize() != 0 );

    auto faces = loadBuffer_<DIRTY_FACE>();
    facesIndicesBuffer_.loadDataOpt( GL_ELEMENT_ARRAY_BUFFER, faces.dirty(), faces );

    GL_EXEC( glActiveTexture( GL_TEXTURE0 ) );
    GL_EXEC( glBindTexture( GL_TEXTURE_2D, texture_ ) );
    if ( dirty_ & DIRTY_TEXTURE )
    {
        const auto& texture = objMesh_->getTexture();
        int warp;
        switch ( texture.warp )
        {
        default:
        case MeshTexture::WarpType::Clamp:
            warp = GL_CLAMP_TO_EDGE;
            break;
        case MeshTexture::WarpType::Repeat:
            warp = GL_REPEAT;
            break;
        case MeshTexture::WarpType::Mirror:
            warp = GL_MIRRORED_REPEAT;
            break;
        }
        int filter = texture.filter == MeshTexture::FilterType::Linear ? GL_LINEAR : GL_NEAREST;
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, warp ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, warp ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ) );
        GL_EXEC( glPixelStorei( GL_UNPACK_ALIGNMENT, 1 ) );
        GL_EXEC( glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, texture.resolution.x, texture.resolution.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture.pixels.data() ) );
    }
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "tex" ), 0 ) );

    // Diffuse
    GL_EXEC( glActiveTexture( GL_TEXTURE1 ) );
    GL_EXEC( glBindTexture( GL_TEXTURE_2D, faceColorsTex_ ) );
    if ( dirty_ & DIRTY_PRIMITIVE_COLORMAP )
    {
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST ) );
        GL_EXEC( glPixelStorei( GL_UNPACK_ALIGNMENT, 1 ) );

        auto facesColorMap = objMesh_->getFacesColorMap();
        auto res = calcTextureRes( int( facesColorMap.size() ), maxTexSize_ );
        facesColorMap.resize( res.x * res.y );
        GL_EXEC( glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, res.x, res.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, facesColorMap.data() ) );
    }
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "faceColors" ), 1 ) );

    // Normals
    auto faceNormals = loadBuffer_<DIRTY_FACES_RENDER_NORMAL>();
    GL_EXEC( glActiveTexture( GL_TEXTURE2 ) );
    GL_EXEC( glBindTexture( GL_TEXTURE_2D, facesNormalsTex_ ) );
    if ( faceNormals.dirty() )
    {
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST ) );
        GL_EXEC( glPixelStorei( GL_UNPACK_ALIGNMENT, 1 ) );

        auto res = calcTextureRes( int( faceNormals.glSize() ), maxTexSize_ );
        assert( res.x * res.y == faceNormals.glSize() );
        GL_EXEC( glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, res.x, res.y, 0, GL_RGBA, GL_FLOAT, faceNormals.data() ) );
    }
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "faceNormals" ), 2 ) );

    // Selection
    auto faceSelection = loadBuffer_<DIRTY_SELECTION>();
    GL_EXEC( glActiveTexture( GL_TEXTURE3 ) );
    GL_EXEC( glBindTexture( GL_TEXTURE_2D, faceSelectionTex_ ) );
    if ( faceSelection.dirty() )
    {
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST ) );
        GL_EXEC( glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST ) );
        GL_EXEC( glPixelStorei( GL_UNPACK_ALIGNMENT, 1 ) );

        auto res = calcTextureRes( int( faceSelection.glSize() ), maxTexSize_ );
        assert( res.x * res.y == faceSelection.glSize() );
        GL_EXEC( glTexImage2D( GL_TEXTURE_2D, 0, GL_R32UI, res.x, res.y, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, faceSelection.data() ) );
    }
    GL_EXEC( glUniform1i( glGetUniformLocation( shader, "selection" ), 3 ) );

    dirty_ &= ~DIRTY_MESH;
    dirty_ &= ~DIRTY_VERTS_COLORMAP;
    normalsBound_ = true;
}

void RenderMeshObject::bindMeshPicker_()
{
    auto shader = ShadersHolder::getShaderId( ShadersHolder::Picker );
    GL_EXEC( glBindVertexArray( meshPickerArrayObjId_ ) );
    GL_EXEC( glUseProgram( shader ) );

    auto positions = loadBuffer_<DIRTY_POSITION>();
    bindVertexAttribArray( shader, "position", vertPosBuffer_, positions, 3, positions.dirty(), positions.glSize() != 0 );

    auto faces = loadBuffer_<DIRTY_FACE>();
    facesIndicesBuffer_.loadDataOpt( GL_ELEMENT_ARRAY_BUFFER, faces.dirty(), faces );

    dirty_ &= ~DIRTY_POSITION;
    dirty_ &= ~DIRTY_FACE;
}

void RenderMeshObject::drawMesh_( bool /*solid*/, ViewportId viewportId, bool picker ) const
{
    /* Avoid Z-buffer fighting between filled triangles & wireframe lines */
    GL_EXEC( glEnable( GL_POLYGON_OFFSET_FILL ) );
    if ( ( objMesh_->getVisualizePropertyMask( MeshVisualizePropertyType::Edges )
       // intentionally do not check selected edges and borders since they are typically thicker and include not all edges
       //  | objMesh_->getVisualizePropertyMask( MeshVisualizePropertyType::SelectedEdges )
       //  | objMesh_->getVisualizePropertyMask( MeshVisualizePropertyType::BordersHighlight ) 
        ).contains( viewportId ) )
    {
        // offset triangles further with factor depending on triangle orientation to clearly see edges on top of them
        GL_EXEC( glPolygonOffset( 1.0, 1.0 ) );
    }
    else
    {
        // offset all triangles on fixed amount to avoid halo effect in flat shading mode
        GL_EXEC( glPolygonOffset( 0.0, 1.0 ) );
    }

    if ( !picker )
        getViewerInstance().incrementThisFrameGLPrimitivesCount( Viewer::GLPrimitivesType::TriangleElementsNum, getGLSize_<DIRTY_FACE>() );

    GL_EXEC( glDrawElements( GL_TRIANGLES, int( 3 * getGLSize_<DIRTY_FACE>() ), GL_UNSIGNED_INT, 0 ) );

    GL_EXEC( glDisable( GL_POLYGON_OFFSET_FILL ) );
}

void RenderMeshObject::initBuffers_()
{
    // Mesh: Vertex Array Object & Buffer objects
    GL_EXEC( glGenVertexArrays( 1, &meshArrayObjId_ ) );
    GL_EXEC( glBindVertexArray( meshArrayObjId_ ) );
    GL_EXEC( glGenTextures( 1, &texture_ ) );

    GL_EXEC( glGenTextures( 1, &faceColorsTex_ ) );

    GL_EXEC( glGenTextures( 1, &facesNormalsTex_ ) );

    GL_EXEC( glGenTextures( 1, &faceSelectionTex_ ) );

    GL_EXEC( glGenVertexArrays( 1, &meshPickerArrayObjId_ ) );
    GL_EXEC( glBindVertexArray( meshPickerArrayObjId_ ) );

    GL_EXEC( glGenVertexArrays( 1, &borderArrayObjId_ ) );
    GL_EXEC( glGenBuffers( 1, &borderBufferObjId_ ) );

    GL_EXEC( glGenVertexArrays( 1, &selectedEdgesArrayObjId_ ) );
    GL_EXEC( glGenBuffers( 1, &selectedEdgesBufferObjId_ ) );

    GL_EXEC( glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTexSize_ ) );
    assert( maxTexSize_ > 0 );

    dirty_ = DIRTY_ALL;
    normalsBound_ = false;

    bufferGLSize_.fill( 0 );
}

void RenderMeshObject::freeBuffers_()
{
    if ( !Viewer::constInstance()->isGLInitialized() || !loadGL() )
        return;
    GL_EXEC( glDeleteVertexArrays( 1, &meshArrayObjId_ ) );
    GL_EXEC( glDeleteVertexArrays( 1, &meshPickerArrayObjId_ ) );
    GL_EXEC( glDeleteVertexArrays( 1, &borderArrayObjId_ ) );
    GL_EXEC( glDeleteVertexArrays( 1, &selectedEdgesArrayObjId_ ) );

    GL_EXEC( glDeleteBuffers( 1, &borderBufferObjId_ ) );
    GL_EXEC( glDeleteBuffers( 1, &selectedEdgesBufferObjId_ ) );

    GL_EXEC( glDeleteTextures( 1, &texture_ ) );
    GL_EXEC( glDeleteTextures( 1, &faceColorsTex_ ) );
    GL_EXEC( glDeleteTextures( 1, &faceSelectionTex_ ) );
    GL_EXEC( glDeleteTextures( 1, &facesNormalsTex_ ) );
}

void RenderMeshObject::update_( ViewportId id )
{
    auto mesh = objMesh_->mesh();

    MR_TIMER;
    auto objDirty = objMesh_->getDirtyFlags();
    uint32_t dirtyNormalFlag = objMesh_->getNeededNormalsRenderDirtyValue( id );
    if ( dirtyNormalFlag & DIRTY_FACES_RENDER_NORMAL )
    {
        // vertNormalsBufferObj_ should be valid no matter what normals we use
        if ( !objMesh_->creases().any() )
            dirtyNormalFlag |= DIRTY_VERTS_RENDER_NORMAL;
        else
            dirtyNormalFlag |= DIRTY_CORNERS_RENDER_NORMAL;
    }
    
    // purpose of `normalsBound_` flag:
    //     objDirty == DIRTY_FACES_RENDER_NORMAL
    // call renderPicker:
    //     dirty_ = objDirty;
    //     dirtyNormalFlag == DIRTY_FACES_RENDER_NORMAL | DIRTY_VERTS_RENDER_NORMAL;
    //     dirty_ -= ( DIRTY_RENDER_NORMALS - dirtyNormalFlag ); // dirty_ -= DIRTY_CORNERS_RENDER_NORMAL
    //     vertNormalsBufferObj_ = objVertNormals;
    //     faceNormalsTexture_ = objFaceNormals;
    //     objMesh_->resetDirty();
    //     // no bind normals because picker doesn't need it
    // call render:
    //     dirty_ = objDirty;
    //     dirtyNormalFlag == 0; // because we copied normals on `renderPicker` call
    //     dirty_ -= ( DIRTY_RENDER_NORMALS - dirtyNormalFlag ); // dirty_ -= DIRTY_RENDER_NORMALS
    //     // no coping of normals (correct)
    //     objMesh_->resetDirty();
    //     // missing bind because !(dirty_ & ( DIRTY_VERTS_RENDER_NORMAL | DIRTY_CORNERS_RENDER_NORMAL ))
    // 
    // `normalsBound_` flag saves this case
    if ( dirtyNormalFlag )
        normalsBound_ = false;

    dirty_ |= objDirty;

    if ( normalsBound_ )
        dirty_ &= ~( DIRTY_RENDER_NORMALS - dirtyNormalFlag ); // it does not affect copy, `dirtyNormalFlag` does

    if ( objMesh_->getColoringType() != ColoringType::VertsColorMap )
        dirty_ &= ~DIRTY_VERTS_COLORMAP;

    if ( dirty_ & DIRTY_FACE )
        dirty_ |= DIRTY_EDGE;

    objMesh_->resetDirtyExeptMask( DIRTY_RENDER_NORMALS - dirtyNormalFlag );
}

void RenderMeshObject::resetBuffers_()
{
    bufferObj_.clear();
}

template <RenderMeshObject::DirtyFlag dirtyFlag>
RenderMeshObject::BufferRef<RenderMeshObject::BufferType<dirtyFlag>> RenderMeshObject::prepareBuffer_( std::size_t glSize, DirtyFlag flagToReset )
{
    using T = BufferType<dirtyFlag>;
    getGLSize_<dirtyFlag>() = glSize;
    auto memSize = sizeof(T) * getGLSize_<dirtyFlag>();
    if ( bufferObj_.size() < memSize )
        bufferObj_.resize( memSize );
    return {
        reinterpret_cast<T*>( bufferObj_.data() ),
        glSize,
        &dirty_,
        flagToReset,
    };
}

template <RenderMeshObject::DirtyFlag dirtyFlag>
RenderMeshObject::BufferRef<RenderMeshObject::BufferType<dirtyFlag>> RenderMeshObject::loadBuffer_()
{
    if constexpr ( dirtyFlag == DIRTY_VERTS_RENDER_NORMAL )
    {
        // bufferObj_ should be valid no matter what normals we use
        if ( !( dirty_ & DIRTY_VERTS_RENDER_NORMAL ) && !( dirty_ & DIRTY_CORNERS_RENDER_NORMAL ) )
            return { nullptr, getGLSize_<dirtyFlag>(), nullptr, 0 };
    }
    else
    {
        if ( !( dirty_ & dirtyFlag ) )
            return { nullptr, getGLSize_<dirtyFlag>(), nullptr, 0 };
    }

    const auto& mesh = objMesh_->mesh();
    auto numF = mesh->topology.lastValidFace() + 1;

    if constexpr ( dirtyFlag == DIRTY_POSITION )
    {
        MR_NAMED_TIMER( "vertbased_dirty_positions" );

        auto buffer = prepareBuffer_<dirtyFlag>( 3 * numF );

        BitSetParallelFor( mesh->topology.getValidFaces(), [&] ( FaceId f )
        {
            auto ind = 3 * f;
            Vector3f v[3];
            mesh->getTriPoints( f, v[0], v[1], v[2] );
            for ( int i = 0; i < 3; ++i )
                buffer[ind + i] = v[i];
        } );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_VERTS_RENDER_NORMAL )
    {
        if ( dirty_ & DIRTY_VERTS_RENDER_NORMAL )
        {
            auto buffer = prepareBuffer_<dirtyFlag>( 3 * numF, DIRTY_VERTS_RENDER_NORMAL );

            MR_NAMED_TIMER( "dirty_vertices_normals" )

            const auto &vertsNormals = objMesh_->getVertsNormals();
            BitSetParallelFor( mesh->topology.getValidFaces(), [&]( FaceId f )
            {
                auto ind = 3 * f;
                VertId v[3];
                mesh->topology.getTriVerts( f, v );
                for ( int i = 0; i < 3; ++i )
                {
                    const auto &norm = vertsNormals[v[i]];
                    buffer[ind + i] = norm;
                }
            } );

            return buffer;
        }
        else if ( dirty_ & DIRTY_CORNERS_RENDER_NORMAL )
        {
            MR_NAMED_TIMER( "dirty_corners_normals" )

            auto buffer = prepareBuffer_<dirtyFlag>( 3 * numF, DIRTY_CORNERS_RENDER_NORMAL );

            const auto& creases = objMesh_->creases();
            const auto cornerNormals = computePerCornerNormals( *mesh, creases.any() ? &creases : nullptr );
            BitSetParallelFor( mesh->topology.getValidFaces(), [&] ( FaceId f )
            {
                auto ind = 3 * f;
                const auto& cornerN = cornerNormals[f];
                for ( int i = 0; i < 3; ++i )
                    buffer[ind + i] = cornerN[i];
            } );

            return buffer;
        }
        else
        {
            assert( false );
            return { nullptr, 0, nullptr, 0 };
        }
    }
    else if constexpr ( dirtyFlag == DIRTY_FACES_RENDER_NORMAL )
    {
        MR_NAMED_TIMER( "dirty_faces_normals" )

        auto res = calcTextureRes( numF, maxTexSize_ );
        assert( res.x * res.y >= numF );
        auto buffer = prepareBuffer_<dirtyFlag>( res.x * res.y );

        computePerFaceNormals4( *mesh, buffer.data(), buffer.size() );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_VERTS_COLORMAP )
    {
        MR_NAMED_TIMER( "vert_colormap" );

        auto buffer = prepareBuffer_<dirtyFlag>( 3 * numF );

        const auto& vertsColorMap = objMesh_->getVertsColorMap();
        BitSetParallelFor( mesh->topology.getValidFaces(), [&] ( FaceId f )
        {
            auto ind = 3 * f;
            VertId v[3];
            mesh->topology.getTriVerts( f, v );
            for ( int i = 0; i < 3; ++i )
                buffer[ind + i] = vertsColorMap[v[i]];
        } );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_FACE )
    {
        auto buffer = prepareBuffer_<dirtyFlag>( numF );

        const auto& edgePerFace = mesh->topology.edgePerFace();
        BitSetParallelForAll( mesh->topology.getValidFaces(), [&] ( FaceId f )
        {
            auto ind = 3 * f;
            if ( f >= numF )
                return;
            if ( !edgePerFace[f].valid() )
                buffer[f] = Vector3i();
            else
                buffer[f] = Vector3i{ ind, ind + 1, ind + 2 };
        } );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_EDGE )
    {
        auto buffer = prepareBuffer_<dirtyFlag>( 3 * numF );

        const auto& edgePerFace = mesh->topology.edgePerFace();
        BitSetParallelForAll( mesh->topology.getValidFaces(), [&] ( FaceId f )
        {
            auto ind = 3 * f;
            if ( f >= numF )
                return;
            if ( !edgePerFace[f].valid() )
            {
                for ( int i = 0; i < 3; ++i )
                    buffer[ind + i] = Vector2i();
            }
            else
            {
                for ( int i = 0; i < 3; ++i )
                    buffer[ind + i] = Vector2i{ ind + i, ind + ( ( i + 1 ) % 3 ) };
            }
        } );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_UV )
    {
        auto numV = mesh->topology.lastValidVert() + 1;
        const auto& uvCoords = objMesh_->getUVCoords();
        if ( objMesh_->getVisualizeProperty( VisualizeMaskType::Texture, ViewportMask::any() ) )
        {
            assert( uvCoords.size() >= numV );
        }
        if ( uvCoords.size() >= numV )
        {
            auto buffer = prepareBuffer_<dirtyFlag>( 3 * numF );

            BitSetParallelFor( mesh->topology.getValidFaces(), [&] ( FaceId f )
            {
                auto ind = 3 * f;
                VertId v[3];
                mesh->topology.getTriVerts( f, v );
                for ( int i = 0; i < 3; ++i )
                    buffer[ind + i] = uvCoords[v[i]];
            } );

            return buffer;
        }
        else
        {
            getGLSize_<dirtyFlag>() = 0;
            return { nullptr, 0, &dirty_, dirtyFlag };
        }
    }
    else if constexpr ( dirtyFlag == DIRTY_SELECTION )
    {
        auto size = numF / 32 + 1;
        auto res = calcTextureRes( size, maxTexSize_ );
        assert( res.x * res.y >= size );
        auto buffer = prepareBuffer_<dirtyFlag>( res.x * res.y );

        const auto& selection = objMesh_->getSelectedFaces().m_bits;
        const unsigned* selectionData = ( unsigned* )selection.data();
        tbb::parallel_for( tbb::blocked_range<int>( 0, ( int )getGLSize_<dirtyFlag>() ), [&] ( const tbb::blocked_range<int>& range )
        {
            for ( int r = range.begin(); r < range.end(); ++r )
            {
                auto& block = buffer[r];
                if ( r / 2 >= selection.size() )
                {
                    block = 0;
                    continue;
                }
                block = selectionData[r];
            }
        } );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_BORDER_LINES )
    {
        auto boundary = mesh->topology.findBoundary();
        size_t size = 0;
        for ( const auto& b : boundary )
            size += 2 * b.size();
        auto buffer = prepareBuffer_<dirtyFlag>( size );

        size_t cur = 0;
        for ( auto& b : boundary )
        {
            for ( auto& e : b )
            {
                buffer[cur++] = mesh->points[mesh->topology.org( e )];
                buffer[cur++] = mesh->points[mesh->topology.dest( e )];
            }
        }
        assert( cur == getGLSize_<dirtyFlag>() );

        return buffer;
    }
    else if constexpr ( dirtyFlag == DIRTY_EDGES_SELECTION )
    {
        auto selectedEdges = objMesh_->getSelectedEdges();
        for ( auto e : selectedEdges )
            if ( !mesh->topology.hasEdge( e ) )
                selectedEdges.reset( e );
        auto buffer = prepareBuffer_<dirtyFlag>( 2 * selectedEdges.count() );

        size_t cur = 0;
        for ( auto e : selectedEdges )
        {
            buffer[cur++] = mesh->orgPnt( e );
            buffer[cur++] = mesh->destPnt( e );
        }
        assert( cur == getGLSize_<dirtyFlag>() );

        return buffer;
    }
}

template<RenderMeshObject::DirtyFlag dirtyFlag>
std::size_t &RenderMeshObject::getGLSize_()
{
    constexpr auto i = highestBit( dirtyFlag );
    assert( dirtyFlag == 1 << i );
    return bufferGLSize_[i];
}

template<RenderMeshObject::DirtyFlag dirtyFlag>
std::size_t RenderMeshObject::getGLSize_() const
{
    constexpr auto i = highestBit( dirtyFlag );
    assert( dirtyFlag == 1 << i );
    return bufferGLSize_[i];
}

MR_REGISTER_RENDER_OBJECT_IMPL( ObjectMeshHolder, RenderMeshObject )

}
