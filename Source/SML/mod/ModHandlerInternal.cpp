#include "ModHandlerInternal.h"
#include "SMLModule.h"
#include "UObjectGlobals.h"
#include "util/Utility.h"
#include "util/Logging.h"
#include "util/picosha2.h"
#include "GameFramework/Actor.h"
#include "actor/InitMod.h"
#include "actor/InitMenu.h"

void iterateDependencies(std::unordered_map<std::wstring, FModLoadingEntry>& loadingEntries,
	std::unordered_map<std::wstring, uint64_t>& modIndices,
	const FModInfo& selfInfo,
	std::vector<std::wstring>& missingDependencies,
	SML::TopologicalSort::DirectedGraph<uint64_t>& sortGraph,
	const std::unordered_map<std::wstring, FVersionRange>& dependencies,
	bool optional);

void finalizeSortingResults(std::unordered_map<uint64_t, std::wstring>& modByIndex,
	std::unordered_map<std::wstring, FModLoadingEntry>& loadingEntries,
	std::vector<uint64_t>& sortedIndices) {
	std::vector<uint64_t> modsToMoveInTheEnd;
	for (uint64_t i = 0; i < sortedIndices.size(); i++) {
		uint64_t modIndex = sortedIndices[i];
		const FModLoadingEntry& loadingEntry = loadingEntries[modByIndex[modIndex]];
		auto dependencies = loadingEntry.modInfo.dependencies;
		if (dependencies.find(TEXT("@ORDER:LAST")) != dependencies.end())
			modsToMoveInTheEnd.push_back(i);
	}
	for (auto& modIndex : modsToMoveInTheEnd) {
		sortedIndices.erase(std::remove(sortedIndices.begin(), sortedIndices.end(), modIndex), sortedIndices.end());
		sortedIndices.push_back(modIndex);
	}
}

void populateSortedModList(std::unordered_map<uint64_t, std::wstring>& modByIndex,
	std::unordered_map<std::wstring, FModLoadingEntry>& loadingEntries,
	std::vector<uint64_t>& sortedIndices,
	std::vector<FModLoadingEntry>& sortedModLoadingList) {
	for (auto& modIndex : sortedIndices) {
		FModLoadingEntry& entry = loadingEntries[modByIndex[modIndex]];
		sortedModLoadingList.push_back(entry);
	}
}

FModLoadingEntry createSMLLoadingEntry() {
	FModLoadingEntry entry;
	entry.isValid = true;
	entry.modInfo = FModInfo::createDummyInfo(TEXT("SML"));
	entry.modInfo.name = TEXT("Satisfactory Mod Loader");
	entry.modInfo.version = getModLoaderVersion();
	entry.modInfo.description = TEXT("Mod Loading & Compatibility layer for Satisfactory");
	entry.modInfo.authors = TEXT("TODO");
	return entry;
}

FModPakLoadEntry CreatePakLoadEntry(const std::wstring& modid) {
	const std::wstring baseInitPath = formatStr(TEXT("/Game/FactoryGame/"), modid);
	const std::wstring modInitPath = formatStr(baseInitPath, TEXT("/InitMod.InitMod_C"));
	const std::wstring menuInitPath = formatStr(baseInitPath, TEXT("/InitMenu.InitMenu_C"));
	TSubclassOf<AInitMod> modInitializerClass = LoadClass<AInitMod>(nullptr, modInitPath.c_str());
	TSubclassOf<AInitMenu> menuInitializerClass = LoadClass<AInitMenu>(nullptr, menuInitPath.c_str());

	FModPakLoadEntry pakEntry{modid};
	if (modInitializerClass != nullptr) {
		//Prevent UClass Garbage Collection
		modInitializerClass->AddToRoot();
		pakEntry.modInitClass = modInitializerClass;
	}
	if (menuInitializerClass != nullptr) {
		//Prevent UClass Garbage Collection
		menuInitializerClass->AddToRoot();
		pakEntry.menuInitClass = menuInitializerClass;
	}
	return pakEntry;
}

std::wstring getModIdFromFile(const path& filePath) {
	std::wstring modId = filePath.filename().generic_wstring();
	//remove extension from file name
	modId.erase(modId.find_last_of(TEXT('.')));
	if (filePath.extension() == TEXT(".dll")) {
		//UE4-SML-Win64-Shipping, Mod ID is the second piece - name of the module
		if (modId.find(TEXT("UE4-")) == 0 && modId.find(TEXT("-Win64-Shipping")) == modId.size() - 15) {
			return modId.substr(4, modId.size() - 15);
		}
		//otherwise load it straight with the same name as file name
		return modId;
	}
	if (filePath.extension() == TEXT(".pak")) {
		//FactoryGame_p.pak, clean priority suffix if it is there
		if (modId.find_last_of(TEXT("_P")) == modId.size() - 2 ||
			modId.find_last_of(TEXT("_p")) == modId.size() - 2) {
			return modId.substr(0, modId.size() - 2);
		}
		//return normal mod id if it doesn't contain suffix
		return modId;
	}
	return modId;
}

std::string createModuleNameFromModId(const std::wstring& modId) {
	//TODO platform-independent way
	//linker uses names with the following schema during linkage
	return convertStr(formatStr(TEXT("UE4-"), modId, TEXT("-Win64-Shipping.dll")).c_str());
}

FileHash hashFileContents(const path& path) {
	std::ifstream f(path.generic_string(), std::ios::binary);
	std::vector<unsigned char> hash(picosha2::k_digest_size);
	picosha2::hash256(f, hash.begin(), hash.end());
	return picosha2::bytes_to_hex_string(hash);
}

path generateTempFilePath(const FileHash& fileHash, const char* extension) {
	path result = SML::getCacheDirectory();
	return path(result / fileHash).replace_extension(extension);
}

bool extractArchiveFile(path& outFilePath, ttvfs::File* obj) {
	std::ofstream outFile(outFilePath, std::ofstream::binary);
	auto buffer_size = 4096;
	if (!obj->open("rb")) {
		throw std::invalid_argument("Failed opening archive object");
	}
	char* buf = new char[buffer_size];
	do {
		size_t bytes = obj->read(buf, buffer_size);
		outFile.write(buf, bytes);
	} while (obj->getpos() < obj->size());
	outFile.close();
	obj->close();
	return true;
}

nlohmann::json readArchiveJson(ttvfs::File* obj) {
	if (!obj->open("rb")) {
		throw std::invalid_argument("Failed opening archive object");
	}
	std::vector<char> buffer(obj->size());
	obj->read(buffer.data(), obj->size());
	obj->close();
	const std::wstring string(buffer.begin(), buffer.end());
	return parseJsonLenient(string);
}

FileHash hashArchiveFileContents(ttvfs::File* obj) {
	if (!obj->open("rb")) {
		throw std::invalid_argument("Failed opening archive object");
	}
	std::vector<char> buffer(obj->size());
	obj->read(buffer.data(), obj->size());
	obj->close();

	std::vector<unsigned char> hash(picosha2::k_digest_size);
	picosha2::hash256(buffer.begin(), buffer.end(), hash.begin(), hash.end());
	return picosha2::bytes_to_hex_string(hash);
}

void extractArchiveObject(ttvfs::Dir& root, const std::string& objectType, const std::string& archivePath, SML::Mod::FModLoadingEntry& loadingEntry, const json& metadata) {
	ttvfs::File* objectFile = root.getFile(archivePath.c_str());
	if (objectFile == nullptr) {
		throw std::invalid_argument("object specified in data.json is missing in zip file");
	}

	//extract configuration
	if (objectType == "config") {
		//extract mod configuration into the predefined folder
		path configFilePath = getModConfigFilePath(loadingEntry.modInfo.modid);
		if (!exists(configFilePath)) {
			//only extract it if it doesn't exist already
			extractArchiveFile(configFilePath, objectFile);
		}
		return;
	}

	//extract other files into caches folder
	const FileHash fileHash = hashArchiveFileContents(objectFile);

	path filePath = generateTempFilePath(fileHash, objectType.c_str());
	//if cached file doesn't exist, or file hashes don't match, unpack file and copy it
	if (!exists(filePath) || fileHash != hashFileContents(filePath)) {
		//in case of broken cache file, remove old file
		remove(filePath);
		//unpack file in the temporary directory
		extractArchiveFile(filePath, objectFile);
	}
	if (objectType == "pak") {
		int32 loadingPriority = 0;
		if (!metadata.is_null()) {
			const json loadingPriorityJson = metadata["loading_priority"];
			if (loadingPriorityJson.is_number()) {
				loadingPriorityJson.get_to(loadingPriority);
			}
		}
		const std::wstring pakFilePath = filePath.generic_wstring();
		loadingEntry.pakFiles.push_back(FModPakFileEntry{ pakFilePath, loadingPriority });
	} else if (objectType == "sml_mod") {
		if (!loadingEntry.dllFilePath.empty())
			throw std::invalid_argument("mod can only have one DLL module at a time");
		loadingEntry.dllFilePath = filePath.generic_wstring();
	} else if (objectType == "core_mod") {
		throw std::invalid_argument("core mods are not supported by this version of SML");
	} else {
		throw std::invalid_argument("Unknown archive object type encountered");
	}
}

void extractArchiveObjects(ttvfs::Dir& root, const nlohmann::json& dataJson, SML::Mod::FModLoadingEntry& loadingEntry) {
	const nlohmann::json& objects = dataJson["objects"];
	if (!objects.is_array()) {
		throw std::invalid_argument("missing `objects` array in data.json");
	}
	for (auto& value : objects.items()) {
		const nlohmann::json object = value.value();
		if (!object.is_object() ||
			!object["type"].is_string() ||
			!object["path"].is_string()) {
			throw std::invalid_argument("one of object entries in data.json has invalid format");
		}
		std::string objType = object["type"].get<std::string>();
		std::string path = object["path"].get<std::string>();
		extractArchiveObject(root, objType, path, loadingEntry, object["metadata"]);
	}
}

void iterateDependencies(std::unordered_map<std::wstring, FModLoadingEntry>& loadingEntries,
	std::unordered_map<std::wstring, uint64_t>& modIndices,
	const FModInfo& selfInfo,
	std::vector<std::wstring>& missingDependencies,
	TopologicalSort::DirectedGraph<uint64_t>& sortGraph,
	const std::unordered_map<std::wstring, FVersionRange>& dependencies,
	bool optional) {

	for (auto& pair : dependencies) {
		FModLoadingEntry& dependencyEntry = loadingEntries[pair.first];
		FModInfo& depInfo = dependencyEntry.modInfo;
		if (!dependencyEntry.isValid || !pair.second.matches(depInfo.version)) {
			const std::wstring reason = dependencyEntry.isValid ? formatStr(TEXT("unsupported version: "), depInfo.version.string()) : TEXT("not installed");
			const std::wstring message = formatStr(selfInfo.modid, " requires ", pair.first, "(", pair.second.string(), "): ", reason);
			if (!optional) missingDependencies.push_back(message);
			continue;
		}
		sortGraph.addEdge(modIndices[selfInfo.modid], modIndices[depInfo.modid]);
	}
}

IModuleInterface* InitializeSMLModule() {
	return new FSMLModule();
}