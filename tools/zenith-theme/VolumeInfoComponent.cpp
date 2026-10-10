#include "VolumeInfoComponent.h"
#include "ThemeData.h"
#include "PowerSaver.h"
#include "components/TextComponent.h"
#include "LocaleES.h"
#include "VolumeControl.h"
#include "Window.h"
#include "renderers/Renderer.h"
#include "resources/Font.h"

#define VISIBLE_TIME		2650
#define FADE_TIME			250
#define CHECKVOLUMEDELAY	40

// Colour with its alpha multiplied by opacity (0-255).
static unsigned int withOpacity(unsigned int color, int opacity)
{
	unsigned int a = ((color & 0xFF) * (unsigned int)opacity) / 255;
	return (color & 0xFFFFFF00) | a;
}

static const char* volumeGlyph(int volume)
{
	// FontAwesome 4: f026 volume-off, f027 volume-down, f028 volume-up
	if (volume <= 0)
		return "\xEF\x80\xA6";
	if (volume < 50)
		return "\xEF\x80\xA7";
	return "\xEF\x80\xA8";
}

VolumeInfoComponent::VolumeInfoComponent(Window* window, bool actionLine)
	: GuiComponent(window)
{
	mDisplayTime = -1;
	mVolume = -1;
	mCheckTime = 0;

	auto theme = ThemeData::getMenuTheme();
	const auto& vt = theme->Volume;

	const float sw = (float) Renderer::getScreenWidth();
	const float sh = (float) Renderer::getScreenHeight();

	float w, h;
	if (vt.size.x() > 0 && vt.size.y() > 0)
	{
		w = vt.size.x() * sw;
		h = vt.size.y() * sh;
	}
	else
	{
		h = Math::max(30.0f, sh * 0.058f);
		w = Math::max(h * 5.0f, sw * 0.30f);
	}
	setSize(w, h);

	float radius = Math::min(0.5f, Math::max(0.0f, vt.radius));
	mRadius = h * radius;

	mBgColor = vt.color;
	mBarColor = vt.barColor;
	mTrackColor = vt.trackColor;

	mPad = h * 0.5f;
	mBarH = Math::max(4.0f, h * 0.14f);

	auto font = vt.font;
	if (font == nullptr)
		font = theme->TextSmall.font;

	// icon (FontAwesome glyph) on the left
	const float iconSize = Math::round(h * 0.46f);
	auto iconFont = Font::get((int) iconSize, ":/fontawesome-webfont.ttf");
	const float iconW = Math::round(iconSize * 1.25f);
	mIcon = new TextComponent(mWindow, volumeGlyph(100), iconFont, vt.textColor, ALIGN_CENTER);
	mIcon->setPosition(mPad * 0.8f, 0);
	mIcon->setSize(iconW, h);
	addChild(mIcon);

	// percentage on the right
	const float labelW = font->sizeText("100%").x() + 2;
	mLabel = new TextComponent(mWindow, "", font, vt.textColor, ALIGN_RIGHT);
	mLabel->setVerticalAlignment(ALIGN_CENTER);
	mLabel->setPosition(w - mPad - labelW, 0);
	mLabel->setSize(labelW, h);
	addChild(mLabel);

	mBarX0 = mPad * 0.8f + iconW + h * 0.28f;
	mBarX1 = w - mPad - labelW - h * 0.28f;

	const float posX = vt.pos.x() * sw - vt.origin.x() * w;
	const float posY = vt.pos.y() * sh - vt.origin.y() * h;
	setPosition(posX, posY, 0);
}

VolumeInfoComponent::~VolumeInfoComponent()
{
	delete mLabel;
	delete mIcon;
}

void VolumeInfoComponent::update(int deltaTime)
{
	GuiComponent::update(deltaTime);

	if (mDisplayTime >= 0)
	{
		mDisplayTime += deltaTime;
		if (mDisplayTime > VISIBLE_TIME + FADE_TIME)
		{
			mDisplayTime = -1;

			if (isVisible())
			{
				setVisible(false);
				PowerSaver::resume();
			}
		}
	}

	mCheckTime += deltaTime;
	if (mCheckTime < CHECKVOLUMEDELAY)
		return;

	mCheckTime = 0;

	int volume = VolumeControl::getInstance()->getVolume();
	if (volume != mVolume)
	{
		bool firstTime = (mVolume < 0);

		mVolume = volume;

		mLabel->setText(std::to_string(mVolume) + "%");
		mIcon->setText(volumeGlyph(mVolume));

		if (!firstTime)
		{
			mDisplayTime = 0;

			if (!isVisible())
			{
				setVisible(true);
				PowerSaver::pause();
			}
		}
	}
}

void VolumeInfoComponent::render(const Transform4x4f& parentTrans)
{
	if (!mVisible || mDisplayTime < 0)
		return;

	int opacity = 255 - Math::max(0, (mDisplayTime - VISIBLE_TIME) * 255 / FADE_TIME);
	setOpacity(opacity);

	Transform4x4f trans = parentTrans * getTransform();
	Renderer::setMatrix(trans);

	const float w = getSize().x();
	const float h = getSize().y();

	// soft shadow: many thin, very translucent layers (smooth falloff, no visible steps)
	for (int i = 8; i >= 1; i--)
	{
		const float g = i * 1.2f;
		Renderer::drawRoundRect(-g, 3.0f - g, w + 2.0f * g, h + 2.0f * g, mRadius + g, withOpacity(0x00000004, opacity));
	}

	Renderer::drawRoundRect(0, 0, w, h, mRadius, withOpacity(mBgColor, opacity));

	// bar: track and fill
	const float by = (h - mBarH) * 0.5f;
	const float bw = mBarX1 - mBarX0;
	if (bw > 0)
	{
		Renderer::drawRoundRect(mBarX0, by, bw, mBarH, mBarH * 0.5f, withOpacity(mTrackColor, opacity));

		float fill = bw * Math::max(0, Math::min(100, mVolume)) / 100.0f;
		if (fill >= mBarH)
			Renderer::drawRoundRect(mBarX0, by, fill, mBarH, mBarH * 0.5f, withOpacity(mBarColor, opacity));
		else if (fill > 0.5f)
			Renderer::drawRoundRect(mBarX0, by, mBarH, mBarH, mBarH * 0.5f, withOpacity(mBarColor, opacity));
	}

	GuiComponent::render(parentTrans);
}
