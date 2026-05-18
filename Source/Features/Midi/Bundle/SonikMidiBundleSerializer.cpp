#include "SonikMidiBundleSerializer.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>

namespace sonik::midi
{
    //--------------------------------------------------------------------------
    juce::var SonikMidiBundleSerializer::sortedClone (const juce::var& v)
    {
        if (v.isArray())
        {
            juce::Array<juce::var> out;
            if (const auto* arr = v.getArray())
            {
                out.ensureStorageAllocated (arr->size());
                for (const auto& e : *arr)
                    out.add (sortedClone (e));
            }
            return juce::var (out);
        }

        if (auto* obj = v.getDynamicObject())
        {
            std::vector<juce::Identifier> keys;
            const auto& props = obj->getProperties();
            keys.reserve ((size_t) props.size());
            for (int i = 0; i < props.size(); ++i)
                keys.push_back (props.getName (i));

            std::sort (keys.begin(), keys.end(),
                       [] (const juce::Identifier& a, const juce::Identifier& b)
                       { return a.toString().compare (b.toString()) < 0; });

            auto* fresh = new juce::DynamicObject();
            for (const auto& k : keys)
                fresh->setProperty (k, sortedClone (obj->getProperty (k)));
            return juce::var (fresh);
        }

        // Primitives (bool/int/int64/double/string/undefined/void) are
        // value-copied by juce::var's value semantics.
        return v;
    }

    //--------------------------------------------------------------------------
    juce::String SonikMidiBundleSerializer::sha256OfSortedJson (const juce::var& v)
    {
        const auto sorted = sortedClone (v);
        const auto text   = juce::JSON::toString (sorted, /*allOnOneLine*/ true);
        const auto utf8   = text.toRawUTF8();
        const auto length = (size_t) text.getNumBytesAsUTF8();
        return juce::SHA256 (utf8, length).toHexString();
    }

    //--------------------------------------------------------------------------
    juce::var SonikMidiBundleSerializer::toJson (const SonikMidiBundle& bundle)
    {
        auto* root = new juce::DynamicObject();

        auto* manifest = new juce::DynamicObject();
        manifest->setProperty ("appVersion",                 bundle.manifest.appVersion);
        manifest->setProperty ("sonikSchemaVersionAtExport", bundle.manifest.sonikSchemaVersionAtExport);
        manifest->setProperty ("exportedAt",                 bundle.manifest.exportedAtIso8601);
        manifest->setProperty ("sha256",                     bundle.manifest.sha256);
        manifest->setProperty ("bundleEnvelopeVersion",      kBundleEnvelopeVersion);
        if (bundle.manifest.exporterDeviceName.isNotEmpty())
            manifest->setProperty ("exporterDeviceName", bundle.manifest.exporterDeviceName);

        root->setProperty ("manifest", juce::var (manifest));
        root->setProperty ("mapping",  bundle.mappingJson);
        return juce::var (root);
    }

    //--------------------------------------------------------------------------
    BundleResult<SonikMidiBundle, BundleParseError>
    SonikMidiBundleSerializer::fromJson (const juce::var& root)
    {
        if (! root.isObject())
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "root is not a JSON object", {} });

        const auto manifestVar = root.getProperty ("manifest", juce::var());
        const auto mappingVar  = root.getProperty ("mapping",  juce::var());

        if (! manifestVar.isObject())
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "manifest block missing or not an object", "manifest" });

        if (! mappingVar.isObject())
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "mapping block missing or not an object", "mapping" });

        SonikMidiBundle out;
        out.mappingJson = mappingVar;

        auto requireString = [&] (const char* key, juce::String& dst) -> bool
        {
            const auto v = manifestVar.getProperty (key, juce::var());
            if (! v.isString())
                return false;
            dst = v.toString();
            return dst.isNotEmpty();
        };

        if (! requireString ("appVersion", out.manifest.appVersion))
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "manifest field missing", "appVersion" });

        const auto schemaVar = manifestVar.getProperty ("sonikSchemaVersionAtExport", juce::var());
        if (! (schemaVar.isInt() || schemaVar.isInt64()))
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "manifest field missing", "sonikSchemaVersionAtExport" });
        out.manifest.sonikSchemaVersionAtExport = static_cast<int> (schemaVar);

        if (! requireString ("exportedAt", out.manifest.exportedAtIso8601))
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "manifest field missing", "exportedAt" });

        if (! requireString ("sha256", out.manifest.sha256))
            return BundleResult<SonikMidiBundle, BundleParseError>::failure (
                BundleParseError { "manifest field missing", "sha256" });

        // Optional.
        const auto exporterDev = manifestVar.getProperty ("exporterDeviceName", juce::var());
        if (exporterDev.isString())
            out.manifest.exporterDeviceName = exporterDev.toString();

        return BundleResult<SonikMidiBundle, BundleParseError>::success (std::move (out));
    }
}
