#include "gepch.h"
#include "AssetMeta.h"

#include <fstream>
#include <sstream>
#include <yaml-cpp/yaml.h>

namespace GanymedE {

	namespace AssetMetaSerializer {

		std::filesystem::path SidecarPath(const std::filesystem::path& assetPath)
		{
			std::filesystem::path sidecar = assetPath;
			sidecar += ".meta";
			return sidecar;
		}

		bool IsSidecarPath(const std::filesystem::path& path)
		{
			// `.bad` too: a quarantined sidecar is still not an asset, and `.bad` is not a
			// recognized extension anyway - this exists so the intent is readable at the call
			// site rather than implied by AssetTypeFromExtension returning None.
			std::string extension = path.extension().string();
			return extension == ".meta" || extension == ".bad";
		}

		ReadResult Read(const std::filesystem::path& assetFullPath, AssetMeta& out)
		{
			std::filesystem::path sidecarPath = SidecarPath(assetFullPath);
			if (!std::filesystem::exists(sidecarPath))
				return ReadResult::Missing;

			std::stringstream buffer;
			{
				// Scoped so the handle is closed before a caller can quarantine the file:
				// Windows refuses to rename a file that is still open.
				std::ifstream stream(sidecarPath);
				if (!stream)
					return ReadResult::Corrupt;

				buffer << stream.rdbuf();
			}

			AssetMeta meta;
			try
			{
				// Everything that can throw is inside one try, including the scalar
				// conversions: `as<uint64_t>()` on a non-numeric Handle throws just as loudly
				// as a syntax error, and both mean the same thing to the caller.
				YAML::Node root = YAML::Load(buffer.str());
				YAML::Node node = root["Asset"];
				if (!node)
					return ReadResult::Corrupt;

				YAML::Node handleNode = node["Handle"];
				if (!handleNode)
					return ReadResult::Corrupt;

				meta.Handle = AssetHandle(handleNode.as<uint64_t>());
				if (!IsAssetHandleValid(meta.Handle))
					return ReadResult::Corrupt;   // a zero handle is no identity at all

				// A name this build does not know is not corruption - a sidecar written by a
				// newer engine is expected to survive a round trip through an older one. The
				// caller re-derives the type from the extension and keeps the handle.
				if (YAML::Node typeNode = node["Type"])
					meta.Type = AssetTypeFromString(typeNode.as<std::string>());

				if (YAML::Node versionNode = node["ImportConfigVersion"])
					meta.ImportConfigVersion = versionNode.as<int>();

				if (YAML::Node configNode = node["Config"])
				{
					for (const auto& entry : configNode)
					{
						// Scalars only, by design (see AssetMeta::Config). A nested value is
						// dropped rather than mangled into a string, because writing it back
						// wrong is worse than not carrying it.
						if (entry.first.IsScalar() && entry.second.IsScalar())
							meta.Config[entry.first.as<std::string>()] = entry.second.as<std::string>();
					}
				}
			}
			catch (const YAML::Exception&)
			{
				// The message is not logged here: this returns Corrupt and the caller has the
				// asset path, which is what a reader needs. Reporting in both places would
				// double every warning during a scan.
				return ReadResult::Corrupt;
			}

			out = meta;
			return ReadResult::Ok;
		}

		bool Write(const std::filesystem::path& assetFullPath, const AssetMeta& meta)
		{
			std::filesystem::path sidecarPath = SidecarPath(assetFullPath);

			// Fixed key order, and Config emitted even when empty: a write -> read -> write
			// round trip is byte-identical, the same discipline `.gmat` and canonical scene
			// saves are held to. An empty `Config: {}` also shows a human where Phase 4's
			// import settings will land.
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "Asset" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "Handle" << YAML::Value << static_cast<uint64_t>(meta.Handle);
			out << YAML::Key << "Type" << YAML::Value << AssetTypeToString(meta.Type);
			out << YAML::Key << "ImportConfigVersion" << YAML::Value << meta.ImportConfigVersion;

			out << YAML::Key << "Config" << YAML::Value;
			if (meta.Config.empty())
				out << YAML::Flow;
			out << YAML::BeginMap;
			for (const auto& [key, value] : meta.Config)
				out << YAML::Key << key << YAML::Value << value;
			out << YAML::EndMap;

			out << YAML::EndMap;
			out << YAML::EndMap;

			std::filesystem::path tempPath = sidecarPath;
			tempPath += ".tmp";

			{
				std::ofstream fout(tempPath);
				if (!fout)
				{
					GE_CORE_ERROR("Could not open '{0}' for writing - asset identity for '{1}' "
						"was not saved", tempPath.string(), assetFullPath.string());
					return false;
				}

				// yaml-cpp's emitter does not terminate its last line. Newline-terminate it
				// here, unlike the scene and prefab writers: `.meta` is the file whose whole
				// reason to exist is being committed and merged per asset, and a file with no
				// newline at EOF turns every appended key into a two-line diff.
				fout << out.c_str() << '\n';
			}

			std::error_code ec;
			std::filesystem::rename(tempPath, sidecarPath, ec);
			if (ec)
			{
				GE_CORE_ERROR("Could not replace '{0}' - asset identity for '{1}' was not saved: {2}",
					sidecarPath.string(), assetFullPath.string(), ec.message());
				std::filesystem::remove(tempPath, ec);
				return false;
			}

			return true;
		}

		bool Quarantine(const std::filesystem::path& assetFullPath)
		{
			std::filesystem::path sidecarPath = SidecarPath(assetFullPath);
			std::filesystem::path quarantinePath = sidecarPath;
			quarantinePath += ".bad";

			std::error_code ec;
			std::filesystem::rename(sidecarPath, quarantinePath, ec);
			return !ec;
		}

	}

}
