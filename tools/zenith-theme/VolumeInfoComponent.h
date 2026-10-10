#pragma once

#include <mutex>
#include "GuiComponent.h"

class TextComponent;
class Window;

// Volume popup. Zenith 0009: horizontal pill (icon, bar, percentage) at the top center by default, coloured from the
// menu theme; a theme can fully restyle it with <menuVolume name="menuvolume"> in the "menu" view.
class VolumeInfoComponent : public GuiComponent
{
public:
	VolumeInfoComponent(Window* window, bool actionLine = true);
	~VolumeInfoComponent();

	void render(const Transform4x4f& parentTrans) override;
	void update(int deltaTime) override;

	void reset() { mVolume = -1; }

private:
	TextComponent*	mLabel;
	TextComponent*	mIcon;

	int mVolume;

	int mCheckTime;
	int mDisplayTime;

	float mPad;
	float mBarX0;
	float mBarX1;
	float mBarH;
	float mRadius;

	unsigned int mBgColor;
	unsigned int mBarColor;
	unsigned int mTrackColor;
};
