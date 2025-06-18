#pragma once

#include <memory>

#include "AAX_CEffectGUI.h"

#include "platform_view.h"

class Aax_gui : public AAX_CEffectGUI {
public:

    static AAX_IEffectGUI* AAX_CALLBACK Create() { return new Aax_gui; }

protected:

    void CreateViewContents() override {}

    void CreateViewContainer() override
    {
        if (auto* parent = GetViewContainerPtr()) {
            platform_view = CreatePlatformView(_delegate.get());
            AttachPlatformView(parent, platform_view);
        }
    }

    void DeleteViewContainer() override
    {
        DestroyPlatformView(platform_view);
    }

    AAX_Result GetViewSize(AAX_Point* view_size) const override
    {
        auto size = _delegate->getSize();
        view_size->horz = size.width;
        view_size->vert = size.height;
        return AAX_SUCCESS;
    }

private:

    std::unique_ptr<Graphics_delegate> _delegate = std::make_unique<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    void* platform_view{nullptr};

};