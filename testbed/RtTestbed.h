#pragma once

#include <string>

#include "Scenario.h"

namespace udsdx
{
	struct Time;
	class SceneObject;
}

// Agent-facing visual-quality testbed for the raytracing renderer. Drives a fixed camera through
// a table of test cases over the Sponza + DamagedHelmet scene built in main.cpp, captures the HDR
// resolve target and the tonemapped back buffer, evaluates artifact checks in-process and writes
// testbed-results/report.json + summary.txt.
namespace rttest
{
	struct Options
	{
		std::wstring OutDir = L"testbed-results";
		// Every run is scenario-driven; the default is the committed regression suite. Relative
		// paths resolve against the launch directory, because the scenario is parsed before
		// UpdownStudio::Initialize moves the cwd to the repo root -- the runner script and the VS
		// debugger working directory both make that the repo root already.
		std::wstring ScenarioPath = L"testbed\\scenarios\\rt-suite.json";
		// Empty runs every case; otherwise only the case with this exact name (plus the cases its
		// "requires" list pulls in).
		std::string CaseFilter;
		// Effective RaytracingMaxSamplesStatic for the run; convergence frame counts derive
		// from it, so lowering it shortens the suite proportionally.
		unsigned int MaxSamples = 256u;
		bool Quick = false;
		// Runs deliberate defect injections and succeeds only if the checks catch them.
		bool SelfTest = false;
		// --pose x,y,z,yaw,pitch overrides the built-in camera pose for one run. Exists so the
		// pose can be re-tuned against captured PNGs without a rebuild; the committed default is
		// what the thresholds are calibrated against, so a run with an override is exploratory.
		bool PoseOverride = false;
		float Pose[5] = {};
	};

	// Fills `options` from the process command line
	// (--scenario FILE --out DIR --case NAME --max-samples N --quick --self-test).
	void ParseCommandLine(Options& options);

	// Hands the parsed scenario to the driver. Call exactly once, after ParseCommandLine and
	// before the scene is built: it seeds the camera defaults ApplyCameraPose uses and the
	// report's scene name. The driver takes ownership -- case names are referenced by pointer for
	// the rest of the process.
	void SetScenario(Scenario&& scenario);

	// The scenario handed to SetScenario. Valid from then on; the scene builder reads it.
	const Scenario& ActiveScenario();

	// The camera pose every case renders from. main.cpp applies it once at startup so the first
	// frame is already correct; the driver re-applies it per case.
	void ApplyCameraPose(udsdx::SceneObject* cameraObject);

	// Applies deterministic render options and builds the case table. Call after the scene is
	// fully built (Core initialized) and before UpdownStudio::Run.
	void Configure(const Options& options, udsdx::SceneObject* cameraObject);

	// The per-frame driver; register as the update callback.
	void Update(const udsdx::Time& time);

	// 0 = all checks passed, 1 = a check failed (or an injected defect went uncaught in
	// --self-test), 2 = skipped (DXR unavailable / RT never activated), 3 = internal error or
	// frame-budget timeout.
	int ExitCode();
}
