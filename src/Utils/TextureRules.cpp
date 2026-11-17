#include "TextureRules.h"

#include "Utils/FileSystem.h"

namespace Util::TextureRules
{
	std::string ToLower(std::string_view a_text)
	{
		std::string text(a_text);
		std::ranges::transform(text, text.begin(), [](char a_char) {
			return a_char == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(a_char)));
		});
		return text;
	}

	std::string NormalizePath(const char* a_path)
	{
		std::string path = ToLower(a_path);
		if (path.starts_with("data\\"))
			path.erase(0, 5);
		if (!path.starts_with("textures\\"))
			path.insert(0, "textures\\");
		if (path.starts_with("textures\\pbr\\"))
			path.erase(9, 4);
		return path;
	}

	bool WildcardMatch(std::string_view a_text, std::string_view a_pattern)
	{
		while (!a_pattern.empty()) {
			if (a_pattern.starts_with("**")) {
				a_pattern.remove_prefix(2);
				if (a_pattern.empty())
					return true;
				if (a_pattern.front() == '\\' && WildcardMatch(a_text, a_pattern.substr(1)))
					return true;
				for (size_t index = 0; index <= a_text.size(); ++index)
					if (WildcardMatch(a_text.substr(index), a_pattern))
						return true;
				return false;
			}
			if (a_pattern.front() == '*') {
				a_pattern.remove_prefix(1);
				for (size_t index = 0; index <= a_text.size(); ++index) {
					if (WildcardMatch(a_text.substr(index), a_pattern))
						return true;
					if (index < a_text.size() && a_text[index] == '\\')
						break;
				}
				return false;
			}
			if (a_text.empty() || (a_pattern.front() == '?' ? a_text.front() == '\\' : a_pattern.front() != a_text.front()))
				return false;
			a_pattern.remove_prefix(1);
			a_text.remove_prefix(1);
		}
		return a_text.empty();
	}

	bool MatchesAny(std::string_view a_path, const std::vector<std::string>& a_patterns)
	{
		return std::ranges::any_of(a_patterns, [&](const std::string& a_pattern) { return WildcardMatch(a_path, a_pattern); });
	}

	std::vector<std::string> ReadPatterns(const json& a_object, const char* a_key)
	{
		std::vector<std::string> patterns;
		const auto it = a_object.find(a_key);
		if (it == a_object.end())
			return patterns;
		if (it->is_string()) {
			patterns.push_back(ToLower(it->get<std::string>()));
		} else if (it->is_array()) {
			for (const auto& value : *it)
				if (value.is_string() && !value.get<std::string>().empty())
					patterns.push_back(ToLower(value.get<std::string>()));
		}
		return patterns;
	}

	float ReadNumber(const json& a_object, const char* a_key, float a_default, float a_min, float a_max)
	{
		const auto it = a_object.find(a_key);
		const float value = it != a_object.end() && it->is_number() ? it->get<float>() : a_default;
		return std::isfinite(value) ? std::clamp(value, a_min, a_max) : a_default;
	}

	std::vector<std::filesystem::path> ListFiles(std::string_view a_folder, std::string_view a_extension)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error;
		std::filesystem::directory_iterator it(std::filesystem::path(a_folder), error);
		for (const std::filesystem::directory_iterator end; !error && it != end; it.increment(error)) {
			std::error_code fileError;
			if (it->is_regular_file(fileError) && ToLower(Util::PathHelpers::ToUtf8(it->path().extension())) == a_extension)
				files.push_back(it->path());
		}
		std::ranges::sort(files);
		return files;
	}
}
