#pragma once

/**
 * Texture-path rules shared by features that pick materials by their diffuse texture (Detail Maps, Ice):
 * path normalization, wildcard patterns and the JSON helpers their rule files use.
 */
namespace Util::TextureRules
{
	/** Lowercase with backslashes. */
	std::string ToLower(std::string_view a_text);

	/** Lowercase, backslashed, rooted at "textures\", with any "textures\pbr\" prefix folded into "textures\". */
	std::string NormalizePath(const char* a_path);

	/** '*' matches within one folder, '**' across folders, '?' one character. Both sides already lowercased. */
	bool WildcardMatch(std::string_view a_text, std::string_view a_pattern);

	bool MatchesAny(std::string_view a_path, const std::vector<std::string>& a_patterns);

	/** A string or array of strings under a_key, lowercased. */
	std::vector<std::string> ReadPatterns(const json& a_object, const char* a_key);

	/** A number under a_key, clamped; a_default when absent or not finite. */
	float ReadNumber(const json& a_object, const char* a_key, float a_default, float a_min, float a_max);

	/** Regular files in a_folder with extension a_extension (lowercase, with the dot), sorted. */
	std::vector<std::filesystem::path> ListFiles(std::string_view a_folder, std::string_view a_extension);
}
