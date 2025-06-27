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
            platform_view = std::make_unique<Platform_view>(_delegate);
            platform_view->receive_parent(parent);
        }
    }

    void DeleteViewContainer() override
    {
        platform_view = nullptr;
    }

    AAX_Result GetViewSize(AAX_Point* view_size) const override
    {
        auto size = _delegate->getSize();
        view_size->horz = size.width;
        view_size->vert = size.height;
        return AAX_SUCCESS;
    }

private:

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    std::unique_ptr<Platform_view> platform_view{nullptr};
    
};