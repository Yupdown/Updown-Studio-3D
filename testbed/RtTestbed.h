#pragma once

#include <string>

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
		// Empty runs every case; otherwise only the case with this exact name.
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
	// (--out DIR --case NAME --max-samples N --quick --self-test).
	void ParseCommandLine(Options& options);

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
