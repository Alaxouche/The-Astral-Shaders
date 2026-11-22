#pragma once

#include "ExtendedEffect.h"

class ENBAdaptation : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbadaptation.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	/** @brief Releases the interior adaptation shader so it recompiles on the next frame it is needed. */
	void ClearShaderCache();

	/** @brief The adaptation texture the latest Execute wrote, or nullptr if it did not write one. */
	ID3D11ShaderResourceView* GetOutputSRV() const;

	TextureManager::Texture textureCurrent;

protected:
	void CreateEffectTextures() override;

private:
	/** @brief Names of the adaptation ping-pong pair for this frame: (previous, output). */
	static std::pair<const char*, const char*> GetTextureNames();

	const char* outputTextureName = nullptr;

	// Two float4s: already a whole number of 16-byte registers, so no alignas (it would pad the effect object)
	struct InteriorCB
	{
		float4 Parameters;  // min, max, sensitivity, time factor
		float4 Options;     // x: clamp to min/max
	};

	/**
	 * @brief Interior adaptation in place of the preset's Draw pass, from the interior values (UseInteriorValues).
	 * @return False when the shader or its buffer is unavailable; the preset's pass then runs.
	 */
	bool DrawInteriorAdaptation(ID3D11ShaderResourceView* a_current, ID3D11ShaderResourceView* a_previous, TextureManager::Texture& a_target);

	ID3D11PixelShader* interiorPS = nullptr;
	bool interiorPSFailed = false;
	winrt::com_ptr<ID3D11Buffer> interiorCB;
	bool interiorActive = false;  // set by UpdateEffectVariables: indoors with UseInteriorValues on
	InteriorCB interiorValues{};

	/** @brief Setting IDs of one set of adaptation values ([ADAPTATION] in enbseries.ini). */
	struct ValueIDs
	{
		uint32_t forceMinMax = 0xFFFFFFFF;
		uint32_t time = 0xFFFFFFFF;
		uint32_t min = 0xFFFFFFFF;
		uint32_t max = 0xFFFFFFFF;
		uint32_t sensitivity = 0xFFFFFFFF;
	};
	ValueIDs exterior;
	ValueIDs interior;  // the *Interior keys, used indoors when UseInteriorValues is on
	uint32_t idUseInterior = 0xFFFFFFFF;
	bool idsCached = false;
};
