#pragma once

#include "MRMesh/MRIRenderObject.h"
#include "MRMesh/MRMeshTexture.h"
#include "MRMesh/MRBuffer.h"
#include "MRRenderGLHelpers.h"
#include "MRRenderHelpers.h"

namespace MR
{
class RenderMeshObject : public IRenderObject
{
public:
    RenderMeshObject( const VisualObject& visObj );
    ~RenderMeshObject();

    virtual void render( const RenderParams& params ) const override;
    virtual void renderPicker( const BaseRenderParams& params, unsigned geomId ) const override;
    virtual size_t heapBytes() const override;

private:
    const ObjectMeshHolder* objMesh_;

    // memory buffer for objects that about to be loaded to GPU
    mutable RenderObjectBuffer bufferObj_;

    mutable std::array<std::size_t, 8 * sizeof( RenderDirtyFlag )> bufferGLSize_; // in bits
    template <RenderDirtyFlag>
    std::size_t& getGLSize_() const;

    template <RenderDirtyFlag dirtyFlag>
    RenderBufferRef<dirtyFlag> prepareBuffer_( std::size_t glSize, RenderDirtyFlag flagToReset = dirtyFlag ) const;
    template <RenderDirtyFlag dirtyFlag>
    RenderBufferRef<dirtyFlag> loadBuffer_() const;

    typedef unsigned int GLuint;

    GLuint borderArrayObjId_{ 0 };
    GLuint borderBufferObjId_{ 0 };

    GLuint selectedEdgesArrayObjId_{ 0 };
    GLuint selectedEdgesBufferObjId_{ 0 };

    GLuint meshArrayObjId_{ 0 };
    GLuint meshPickerArrayObjId_{ 0 };

    mutable GlBuffer vertPosBuffer_;
    mutable GlBuffer vertUVBuffer_;
    mutable GlBuffer vertNormalsBuffer_;
    mutable GlBuffer vertColorsBuffer_;

    mutable GlBuffer facesIndicesBuffer_;
    mutable GlBuffer edgesIndicesBuffer_;
    GLuint texture_{ 0 };

    GLuint faceSelectionTex_{ 0 };

    GLuint faceColorsTex_{ 0 };

    GLuint facesNormalsTex_{ 0 };

    int maxTexSize_{ 0 };

    template <RenderDirtyFlag>
    void renderEdges_( const RenderParams& parameters, GLuint vao, GLuint vbo, const Color& color ) const;

    void renderMeshEdges_( const RenderParams& parameters ) const;

    void bindMesh_( bool alphaSort ) const;
    void bindMeshPicker_() const;

    void drawMesh_( bool solid, ViewportId viewportId, bool picker = false ) const;

    // Create a new set of OpenGL buffer objects
    void initBuffers_();

    // Release the OpenGL buffer objects
    void freeBuffers_();

    void update_( ViewportId id ) const;

    // Marks dirty buffers that need to be uploaded to OpenGL
    mutable RenderDirtyFlag dirty_;
    // this is needed to fix case of missing normals bind (can happen if `renderPicker` before first `render` with flat shading)
    mutable bool normalsBound_{ false };
};

}