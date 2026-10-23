// Standalone check of the [auto-identify] config parser. Build with tests/run_tests.cmd.
#include "../src/auto_identify.cpp"

#include <cstdio>
#include <string>

static int g_failures = 0;
static int g_warnings = 0;

static void Check(const char* name, const char* toml, bool enabled, uint32_t mask, int warnings, uint32_t hideMask = 0) {
	Config cfg;
	g_warnings = 0;
	ParseConfig(toml, cfg, [](const char*) { ++g_warnings; });
	const bool ok = cfg.enabled == enabled && cfg.qualityMask == mask && g_warnings == warnings && cfg.hideNameMask == hideMask;
	std::printf("%s %-28s enabled=%d mask=0x%03x hide=0x%03x warnings=%d\n", ok ? "PASS" : "FAIL", name, cfg.enabled, cfg.qualityMask, cfg.hideNameMask,
	            g_warnings);
	g_failures += ok ? 0 : 1;
}

static constexpr uint32_t Q(uint32_t id) { return 1U << id; }

int main() {
	const uint32_t def = Q(4) | Q(5) | Q(6) | Q(7) | Q(8);
	Check("defaults (no section)", "", true, def, 0);
	Check("shipped config", "[auto-identify]\nenabled = true\nqualities = [\"magic\", \"set\", \"rare\", \"unique\", \"crafted\"]\n", true, def, 0);
	Check("rare+unique only", "[auto-identify]\nqualities = [\"rare\",\"unique\"]\n", true, Q(6) | Q(7), 0);
	Check("numbers and case", "[auto-identify]\nqualities = [4, 'SET', \"Tempered\"]\n", true, Q(4) | Q(5) | Q(9), 0);
	Check("empty list", "[auto-identify]\nqualities = []\n", true, 0, 0);
	Check("multi-line + comments", "[auto-identify]\nqualities = [\n  \"magic\", # blue\n  \"unique\",\n]\n", true, Q(4) | Q(7), 0);
	Check("unknown name skipped", "[auto-identify]\nqualities = [\"magic\", \"legendary\"]\n", true, Q(4), 1);
	Check("disabled", "[auto-identify]\nenabled = false\n", false, def, 0);
	Check("other section ignored", "[other]\nqualities = [\"rare\"]\nenabled = false\n", true, def, 0);
	Check("not an array", "[auto-identify]\nqualities = \"rare\"\n", true, def, 1);
	Check("unterminated array", "[auto-identify]\nqualities = [\"rare\",\n", true, def, 1);
	Check("CRLF line endings", "[auto-identify]\r\nenabled = true\r\nqualities = [\"set\"]\r\n", true, Q(5), 0);
	Check("d2rl section after", "[auto-identify]\nqualities = [\"rare\"]\n[d2rl]\nmatch = [\n \"x\",\n]\n", true, Q(6), 0);
	Check("hide magic only", "[auto-identify]\nhide_magic_affixes = true\n", true, def, 0, Q(4));
	Check("hide rare only", "[auto-identify]\nhide_rare_names = true # comment\n", true, def, 0, Q(6));
	Check("hide all three", "[auto-identify]\nhide_magic_affixes = true\nhide_rare_names = true\nhide_crafted_names = true\n", true, def, 0,
	      Q(4) | Q(6) | Q(8));
	Check("hide explicitly off", "[auto-identify]\nhide_magic_affixes = false\nhide_rare_names = false\n", true, def, 0, 0);
	Check("hide bad value ignored", "[auto-identify]\nhide_rare_names = yes\n", true, def, 0, 0);
	Check("hide in other section", "[other]\nhide_rare_names = true\n", true, def, 0, 0);
	{
		std::string shipped;
		FILE* f = nullptr;
		if (fopen_s(&f, "../config/auto-identify.toml", "rb") == 0 && f != nullptr) {
			char chunk[512];
			for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) {
				shipped.append(chunk, n);
			}
			std::fclose(f);
		}
		if (shipped.empty()) {
			std::printf("FAIL shipped file not found (run from tests/)\n");
			++g_failures;
		} else {
			Check("shipped file on disk", shipped.c_str(), true, def, 0, 0);
		}
	}
	std::printf("%s\n", g_failures == 0 ? "ALL PASSED" : "FAILURES");
	return g_failures == 0 ? 0 : 1;
}
