#ifndef __EMSCRIPTEN__
#include "MRVolumeSegment.h" 
#include "MRVoxelPath.h"
#include "MRObjectVoxels.h"
#include "MRVoxelGraphCut.h"
#include "MRVDBConversions.h"
#include "MRFloatGrid.h"
#include "MRMesh.h"
#include <filesystem>

namespace MR
{

tl::expected<MR::Mesh, std::string> meshFromVoxelsMask( const ObjectVoxels& volume, const VoxelBitSet& mask )
{
    if ( !volume.grid() )
        return tl::make_unexpected( "Cannot create mesh from empty volume." );
    if ( mask.none() )
        return tl::make_unexpected( "Cannot create mesh from empty mask." );

    const auto& indexer = volume.getVolumeIndexer();
    auto expandedMask = mask;
    expandVoxelsMask( expandedMask, indexer, 25 ); // 25 should be in params

    auto accessor = volume.grid()->getConstAccessor();
    double insideAvg = 0.0;
    double outsideAvg = 0.0;
    Box3i partBox;
    for ( auto voxelId : expandedMask )
    {
        auto pos = indexer.toPos( voxelId );
        partBox.include( pos );
        if ( mask.test( voxelId ) )
            insideAvg += double( accessor.getValue( { pos.x,pos.y,pos.z } ) );
        else
            outsideAvg += double( accessor.getValue( { pos.x,pos.y,pos.z } ) );
    }
    insideAvg /= double( mask.count() );
    outsideAvg /= double( expandedMask.count() - mask.count() );
    auto range = float( insideAvg - outsideAvg );

    SimpleVolume voulmePart;
    voulmePart.dims = partBox.size() + Vector3i::diagonal( 1 );

    auto smallExpMask = mask;
    auto smallShrMask = mask;
    expandVoxelsMask( smallExpMask, indexer, 3 );
    shrinkVoxelsMask( smallShrMask, indexer, 3 );


    const int newDimX = voulmePart.dims.x;
    const size_t newDimXY = size_t( newDimX ) * voulmePart.dims.y;
    voulmePart.data.resize( newDimXY * voulmePart.dims.z );
    for ( int z = partBox.min.z; z <= partBox.max.z; ++z )
        for ( int y = partBox.min.y; y <= partBox.max.y; ++y )
            for ( int x = partBox.min.x; x <= partBox.max.x; ++x )
            {
                auto voxId = indexer.toVoxelId( { x,y,z } );
                if ( smallShrMask.test( voxId ) )
                    voulmePart.data[x - partBox.min.x + ( y - partBox.min.y ) * newDimX + ( z - partBox.min.z ) * newDimXY] = 1.0f;
                else if ( smallExpMask.test( voxId ) )
                    voulmePart.data[x - partBox.min.x + ( y - partBox.min.y ) * newDimX + ( z - partBox.min.z ) * newDimXY] =
                        std::clamp( ( accessor.getValue( { x,y,z } ) - float( outsideAvg ) ) / range, 0.0f, 1.0f );
                else
                    voulmePart.data[x - partBox.min.x + ( y - partBox.min.y ) * newDimX + ( z - partBox.min.z ) * newDimXY] = 0.0f;
            }

    auto voxelSize = volume.voxelSize();
    auto grid = simpleVolumeToDenseGrid( voulmePart );
    auto mesh = gridToMesh( grid, voxelSize, 0.5f ).value(); // no callback so cannot b stopped

    for ( auto& p : mesh.points )
    {
        p = p + mult( Vector3f( partBox.min ), voxelSize );
    }

    if ( mesh.topology.numValidFaces() == 0 )
        return tl::make_unexpected( "Failed to create segmented mesh" );

    return mesh;

}

tl::expected<MR::Mesh, std::string> segmentVolume( const ObjectVoxels& volume, const std::vector<std::pair<Vector3f, Vector3f>>& pairs,
                                                   const VolumeSegmentationParameters& params )
{
    VolumeSegmenter segmentator( volume );
    for ( const auto& [start, stop] : pairs )
    {
        VoxelMetricParameters metricParams;
        metricParams.start = size_t( volume.getVoxelIdByPoint( start ) );
        metricParams.stop = size_t( volume.getVoxelIdByPoint( stop ) );
        constexpr char mask = 1;
        for ( int i = 0; i < 4; ++i )
        {
            metricParams.quatersMask = mask << i;
            segmentator.addPathSeeds( metricParams, VolumeSegmenter::SeedType::Inside, params.buildPathExponentModifier );
        }
    }
    auto segmentation = segmentator.segmentVolume( params.segmentationExponentModifier, params.voxelsExpansion );
    if ( !segmentation.has_value() )
        return tl::make_unexpected( segmentation.error() );
    return segmentator.createMeshFromSegmentation( segmentation.value() );
}

// Class implementation

VolumeSegmenter::VolumeSegmenter( const ObjectVoxels& volume ):
    volume_{volume}
{
}

void VolumeSegmenter::addPathSeeds( const VoxelMetricParameters& metricParameters, SeedType seedType, float exponentModifier /*= -1.0f */ )
{
    auto metric = voxelsExponentMetric( volume_, metricParameters, exponentModifier );
    auto path = buildSmallestMetricPath( volume_, metric, metricParameters.start, metricParameters.stop );

    auto& curSeeds = seeds_[seedType];
    auto shift = curSeeds.size();
    curSeeds.resize( shift + path.size() );
    for ( int p = 0; p < path.size(); ++p )
    {
        curSeeds[shift + p] = volume_.getCoordinateByVoxelId( VoxelId( int( path[p] ) ) );
    }
    seedsChanged_ = true;
}

void VolumeSegmenter::setSeeds( const std::vector<Vector3i>& seeds, SeedType seedType )
{
    seeds_[seedType] = seeds;
    seedsChanged_ = true;
}

void VolumeSegmenter::addSeeds( const std::vector<Vector3i>& seeds, SeedType seedType )
{
    auto& curSeeds = seeds_[seedType];
    curSeeds.reserve( curSeeds.size() + seeds.size() );
    curSeeds.insert( curSeeds.end(), seeds.begin(), seeds.end() );
    seedsChanged_ = true;
}

const std::vector<MR::Vector3i>& VolumeSegmenter::getSeeds( SeedType seedType ) const
{
    return seeds_[seedType];
}

tl::expected<VoxelBitSet, std::string> VolumeSegmenter::segmentVolume( float segmentationExponentModifier /*= 3000.0f*/, int voxelsExpansion /*= 25 */ )
{
    if ( seeds_[Inside].empty() )
        return tl::make_unexpected( "No seeds presented" );

    if ( !volume_.grid() )
        return tl::make_unexpected( "Volume contain no grid" );

    if ( seedsChanged_ )
    {
        setupVolumePart_( voxelsExpansion );
        seedsChanged_ = false;
    }

    // Segment volume
    return segmentVolumeByGraphCut( volumePart_, segmentationExponentModifier, seedsInVolumePartSpace_[Inside], seedsInVolumePartSpace_[Outside] );
}

tl::expected<MR::Mesh, std::string> VolumeSegmenter::createMeshFromSegmentation( const VoxelBitSet& segmentation ) const
{
    auto newDims = volumePart_.dims;
    auto newDimXY = newDims.x * newDims.y;
    auto CoordToNewVoxelId = [&] ( const Vector3i& coord )->VoxelId
    {
        return VoxelId( coord.x + coord.y * newDims.x + coord.z * newDimXY );
    };

    auto segmentBlockCopy = volumePart_;
    // Prepare block for meshing
    for ( int z = 0; z < newDims.z; ++z )
        for ( int y = 0; y < newDims.y; ++y )
            for ( int x = 0; x < newDims.x; ++x )
            {
                auto index = CoordToNewVoxelId( Vector3i( x, y, z ) );
                segmentBlockCopy.data[index] = segmentation.test( index ) ? 1.0f : 0.0f;
            }


    auto voxelSize = volume_.voxelSize();
    auto grid = simpleVolumeToDenseGrid( segmentBlockCopy );
    auto mesh = gridToMesh( grid, voxelSize, 0.5f ).value(); // no callback so cannot b stopped


    for ( auto& p : mesh.points )
    {
        p = p + mult( Vector3f( minVoxel_ ), voxelSize );
    }

    if ( mesh.topology.numValidFaces() == 0 )
        return tl::make_unexpected( "Failed to create segmented mesh" );

    return mesh;
}

const MR::Vector3i& VolumeSegmenter::getVolumePartDimensions() const
{
    return volumePart_.dims;
}

const Vector3i& VolumeSegmenter::getMinVoxel() const
{
    return minVoxel_;
}

void VolumeSegmenter::setupVolumePart_( int voxelsExpansion )
{
    auto& curSeeds = seeds_[Inside];
    auto minmaxElemX = std::minmax_element( curSeeds.begin(), curSeeds.end(), []( const Vector3i& first, const Vector3i& second )
    {
        return first.x < second.x;
    } );

    auto minmaxElemY = std::minmax_element( curSeeds.begin(), curSeeds.end(), []( const Vector3i& first, const Vector3i& second )
    {
        return first.y < second.y;
    } );

    auto minmaxElemZ = std::minmax_element( curSeeds.begin(), curSeeds.end(), []( const Vector3i& first, const Vector3i& second )
    {
        return first.z < second.z;
    } );

    auto minVoxel = Vector3i( minmaxElemX.first->x, minmaxElemY.first->y, minmaxElemZ.first->z );
    auto maxVoxel = Vector3i( minmaxElemX.second->x, minmaxElemY.second->y, minmaxElemZ.second->z );

    // need to fix dims: clamp by real voxels bounds
    maxVoxel += Vector3i::diagonal( voxelsExpansion );
    minVoxel -= Vector3i::diagonal( voxelsExpansion );


    const auto& dims = volume_.dimensions();

    maxVoxel.x = std::min( maxVoxel.x, dims.x );
    maxVoxel.y = std::min( maxVoxel.y, dims.y );
    maxVoxel.z = std::min( maxVoxel.z, dims.z );

    minVoxel.x = std::max( minVoxel.x, 0 );
    minVoxel.y = std::max( minVoxel.y, 0 );
    minVoxel.z = std::max( minVoxel.z, 0 );

    bool blockChanged{false};
    if ( minVoxel != minVoxel_ )
    {
        minVoxel_ = minVoxel;
        blockChanged = true;
    }
    if ( maxVoxel != maxVoxel_ )
    {
        maxVoxel_ = maxVoxel;
        blockChanged = true;
    }

    if ( blockChanged )
    {
        volumePart_.dims = maxVoxel - minVoxel + Vector3i::diagonal( 1 );
        const int newDimX = volumePart_.dims.x;
        const size_t newDimXY = size_t( newDimX ) * volumePart_.dims.y;
        volumePart_.data.resize( newDimXY * volumePart_.dims.z );
        const auto& accessor = volume_.grid()->getConstAccessor();
        for ( int z = minVoxel.z; z <= maxVoxel.z; ++z )
            for ( int y = minVoxel.y; y <= maxVoxel.y; ++y )
                for ( int x = minVoxel.x; x <= maxVoxel.x; ++x )
                {
                    volumePart_.data[x - minVoxel.x + ( y - minVoxel.y ) * newDimX + ( z - minVoxel.z ) * newDimXY] =
                        accessor.getValue( {x,y,z} );
                }

        auto minmaxIt = std::minmax_element( volumePart_.data.begin(), volumePart_.data.end() );
        volumePart_.min = *minmaxIt.first;
        volumePart_.max = *minmaxIt.second;


        seedsInVolumePartSpace_[Inside].resize( newDimXY * volumePart_.dims.z );
        seedsInVolumePartSpace_[Outside].resize( newDimXY * volumePart_.dims.z );
    }
    seedsInVolumePartSpace_[Inside].reset();
    seedsInVolumePartSpace_[Outside].reset();

    const int newDimX = volumePart_.dims.x;
    const size_t newDimXY = size_t( newDimX ) * volumePart_.dims.y;

    auto CoordToNewVoxelId = [&]( const Vector3i& coord )->VoxelId
    {
        return VoxelId( coord.x + coord.y * newDimX + coord.z * newDimXY );
    };

    for ( const auto& seed : seeds_[Inside] )
    {
        seedsInVolumePartSpace_[Inside].set( CoordToNewVoxelId( seed - minVoxel_ ) );
    }
    for ( auto seed : seeds_[Outside] )
    {
        seed.x = std::clamp( seed.x, minVoxel_.x, maxVoxel_.x );
        seed.y = std::clamp( seed.y, minVoxel_.y, maxVoxel_.y );
        seed.z = std::clamp( seed.z, minVoxel_.z, maxVoxel_.z );
        seedsInVolumePartSpace_[Outside].set( CoordToNewVoxelId( seed - minVoxel_ ) );
    }
    for ( int i = 0; i < 3; ++i ) // fill non tooth voxels by faces of segment block
    {
        int axis1 = ( i + 1 ) % 3;
        int axis2 = ( i + 2 ) % 3;
        for ( int a1 = 0; a1 < volumePart_.dims[axis1]; ++a1 )
            for ( int a2 = 0; a2 < volumePart_.dims[axis2]; ++a2 )
            {
                Vector3i nearVoxel, farVoxel;
                nearVoxel[i] = 0; nearVoxel[axis1] = a1; nearVoxel[axis2] = a2;
                farVoxel = nearVoxel; farVoxel[i] = volumePart_.dims[i] - 1;
                seedsInVolumePartSpace_[Outside].set( CoordToNewVoxelId( nearVoxel ) );
                seedsInVolumePartSpace_[Outside].set( CoordToNewVoxelId( farVoxel ) );
            }
    }

    seedsInVolumePartSpace_[Outside] -= seedsInVolumePartSpace_[Inside];
}

}
#endif
