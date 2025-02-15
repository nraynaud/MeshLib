#ifndef __EMSCRIPTEN__
#include "MROpenRawVoxelsPlugin.h"
#include "MRViewer/MRRibbonMenu.h"
#include "MRViewer/ImGuiHelpers.h"
#include "MRViewer/MRFileDialog.h"
#include "MRViewer/MRProgressBar.h"
#include "MRMesh/MRObjectVoxels.h"
#include "MRMesh/MRStringConvert.h"
#include "MRViewer/MRAppendHistory.h"
#include "MRMesh/MRChangeSceneAction.h"

namespace
{
constexpr std::array<const char*, size_t( MR::VoxelsLoad::RawParameters::ScalarType::Count )> cScalarTypeNames =
{
    "UInt8",
    "Int8",
    "UInt16",
    "Int16",
    "UInt32",
    "Int32",
    "UInt64",
    "Int64",
    "Float32",
    "Float64"
};
}

namespace MR
{

OpenRawVoxelsPlugin::OpenRawVoxelsPlugin():
    StatePlugin( "Open RAW Voxels" )
{
}

void OpenRawVoxelsPlugin::drawDialog( float menuScaling, ImGuiContext* )
{
    auto menuWidth = 350.0f * menuScaling;
    ImGui::BeginStatePlugin( plugin_name.c_str(), &dialogIsOpen_, menuWidth );

    ImGui::SetNextItemWidth( menuScaling * 200.0f );
    ImGui::DragInt3( "Dimensions", &parameters_.dimensions.x, 1, 0 );
    ImGui::SetNextItemWidth( menuScaling * 100.0f );
    if ( ImGui::DragFloatValid( "Voxel size", &parameters_.voxelSize.x, 1e-3f, 0 ) )
        parameters_.voxelSize = Vector3f::diagonal( parameters_.voxelSize.x );
    ImGui::Separator();
    ImGui::Text( "Scalar type:" );
    for ( int i = 0; i<int( VoxelsLoad::RawParameters::ScalarType::Count ); ++i )
        ImGui::RadioButton( cScalarTypeNames[i], ( int* ) &parameters_.scalarType, i );

    if ( ImGui::Button( "Open file", ImVec2( -1, 0 ) ) )
    {
        auto path = openFileDialog( { {},{},{{"RAW File","*.raw"}} } );
        if ( !path.empty() )
        {
            ProgressBar::orderWithMainThreadPostProcessing( "Load voxels", [params = parameters_, path]()->std::function<void()>
            {
                ProgressBar::nextTask( "Load file" );
                auto res = VoxelsLoad::loadRaw( path, params, ProgressBar::callBackSetProgress );
                if ( res.has_value() )
                {
                    ProgressBar::nextTask( "Create object" );
                    std::shared_ptr<ObjectVoxels> object = std::make_shared<ObjectVoxels>();
                    object->setName( utf8string( path.stem() ) );
                    object->construct( *res, ProgressBar::callBackSetProgress );
                    auto bins = object->histogram().getBins();
                    auto minMax = object->histogram().getBinMinMax( bins.size() / 3 );

                    ProgressBar::nextTask( "Create ISO surface" );
                    object->setIsoValue( minMax.first, ProgressBar::callBackSetProgress );
                    object->select( true );
                    return [object] ()
                    {
                        AppendHistory<ChangeSceneAction>( "Open Voxels", object, ChangeSceneAction::Type::AddObject );
                        SceneRoot::get().addChild( object );
                        getViewerInstance().viewport().preciseFitDataToScreenBorder( { 0.9f } );
                    };
                }
                else
                    return [error = res.error()] ()
                {
                    auto menu = getViewerInstance().getMenuPlugin();
                    if ( menu )
                        menu->showErrorModal( error );
                };
            }, 3 );
        }
    }

    ImGui::End();
}

bool OpenRawVoxelsPlugin::onEnable_()
{
    parameters_ = VoxelsLoad::RawParameters();
    return true;
}

bool OpenRawVoxelsPlugin::onDisable_()
{
    return true;
}

MR_REGISTER_RIBBON_ITEM( OpenRawVoxelsPlugin )

}
#endif
