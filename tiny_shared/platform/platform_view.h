// Create the platform view.
void* CreatePlatformView(float width, float height);

// Destroy the platform view.
void DestroyPlatformView(void* view);

// Attach the platform view to the parent.
void AttachPlatformView(void* parent, void* view);
