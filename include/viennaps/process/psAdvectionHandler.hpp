#pragma once

#include "psProcessContext.hpp"

#include <lsAdvect.hpp>

#include <vcTimer.hpp>

#include <cmath>

namespace viennaps {

VIENNAPS_TEMPLATE_ND(NumericType, D) class AdvectionHandler {
  viennals::Advect<NumericType, D> advectionKernel_;
  viennacore::Timer<> timer_;
  unsigned lsVelOutputCounter_ = 0;
  unsigned totalAdvectionSteps_ = 0;

public:
  ProcessResult initialize(ProcessContext<NumericType, D> &context) {
    // Initialize advection handler with context
    assert(context.translationField);
    auto translationMethod = context.translationField->getTranslationMethod();
    if (translationMethod > 2 || translationMethod < 0) {
      VIENNACORE_LOG_WARNING("Translation field method not supported.");
      return ProcessResult::INVALID_INPUT;
    }

    auto &discSchem = context.advectionParams.spatialScheme;
    if (translationMethod == 1 &&
        (discSchem != SpatialScheme::ENGQUIST_OSHER_1ST_ORDER &&
         discSchem != SpatialScheme::ENGQUIST_OSHER_2ND_ORDER &&
         discSchem != SpatialScheme::LOCAL_LOCAL_LAX_FRIEDRICHS_1ST_ORDER &&
         discSchem != SpatialScheme::LOCAL_LOCAL_LAX_FRIEDRICHS_2ND_ORDER &&
         discSchem != SpatialScheme::WENO_3RD_ORDER &&
         discSchem != SpatialScheme::WENO_5TH_ORDER)) {
      VIENNACORE_LOG_WARNING(
          "Translation field method not supported in combination "
          "with discretization scheme.");
      return ProcessResult::INVALID_INPUT;
    }

    context.resetTime();

    advectionKernel_.setSingleStep(true);
    advectionKernel_.setSpatialScheme(context.advectionParams.spatialScheme);
    advectionKernel_.setTemporalScheme(context.advectionParams.temporalScheme);
    advectionKernel_.setVelocityField(context.translationField);
    advectionKernel_.setTimeStepRatio(context.advectionParams.timeStepRatio);
    advectionKernel_.setSaveAdvectionVelocities(
        context.advectionParams.velocityOutput);
    advectionKernel_.setDissipationAlpha(
        context.advectionParams.dissipationAlpha);
    advectionKernel_.setIgnoreVoids(context.advectionParams.ignoreVoids);
    advectionKernel_.setCheckDissipation(
        context.advectionParams.checkDissipation);
    advectionKernel_.setAdaptiveTimeStepping(
        context.advectionParams.adaptiveTimeStepping,
        context.advectionParams.adaptiveTimeStepSubdivisions);
    advectionKernel_.setLevelSetUpdateExecutor(context.levelSetUpdateExecutor);
    advectionKernel_.setLevelSetRebuildExecutor(
        context.levelSetRebuildExecutor);
    using AdvectFailurePolicy =
        typename viennals::Advect<NumericType, D>::LevelSetUpdateFailurePolicy;
    advectionKernel_.setLevelSetUpdateFailurePolicy(
        context.levelSetUpdateFailurePolicy == LevelSetUpdateFailurePolicy::FAIL
            ? AdvectFailurePolicy::FAIL
            : AdvectFailurePolicy::FALLBACK);

    advectionKernel_.setVelocityUpdateCallback(nullptr);

    // normals vectors are only necessary for analytical velocity fields
    if (translationMethod > 0)
      advectionKernel_.setCalculateNormalVectors(false);

    advectionKernel_.clearLevelSets();
    for (auto &dom : context.domain->getLevelSets()) {
      advectionKernel_.insertNextLevelSet(dom);
    }

    totalAdvectionSteps_ = 0;

    return ProcessResult::SUCCESS;
  }

  void setAdvectionTime(double time) {
    advectionKernel_.setAdvectionTime(time);
  }

  void setVelocityUpdateCallback(
      std::function<bool(SmartPointer<viennals::Domain<NumericType, D>>)>
          callback) {
    advectionKernel_.setVelocityUpdateCallback(callback);
  }

  auto getTotalAdvectionSteps() const { return totalAdvectionSteps_; }

  void disableSingleStep() { advectionKernel_.setSingleStep(false); }

  void prepareAdvection(const ProcessContext<NumericType, D> &context) {
    // Prepare for advection step
    advectionKernel_.prepareLS();
    context.model->initialize(context.domain, context.processTime);
  }

  ProcessResult performAdvection(ProcessContext<NumericType, D> &context) {
    // Perform the advection step.
    //
    // CPU-path reuse rule (intent framework §2.2 item 8): when no Level-Set
    // executor is wired, this function must match the original ViennaPS 4.6.2
    // behavior in code_reference/ViennaPS. Fail-closed guards are only active
    // when a Level-Set update or rebuild executor is present.
    //
    // Use the kernel's own executor state rather than the mutable ProcessContext
    // so that re-initialized handlers and context mutations cannot desync the
    // legacy/fail-closed branch decision.
    const bool hasExecutor = advectionKernel_.hasLevelSetExecutors();

    // Set the maximum advection time.
    if (!context.flags.isALP) {
      advectionKernel_.setAdvectionTime(context.processDuration -
                                        context.processTime);
    }

    timer_.start();
    advectionKernel_.apply();
    timer_.finish();

    if (hasExecutor) {
      if (advectionKernel_.hasLevelSetUpdateError()) {
        viennacore::Logger::getInstance()
            .addError("Level Set update executor failed: " +
                          advectionKernel_.getLevelSetUpdateError(),
                      false)
            .print();
        return ProcessResult::FAILURE;
      }

      if (advectionKernel_.hasLevelSetRebuildError()) {
        viennacore::Logger::getInstance()
            .addError("Level Set rebuild executor failed: " +
                          advectionKernel_.getLevelSetRebuildError(),
                      false)
            .print();
        return ProcessResult::FAILURE;
      }

      if (advectionKernel_.hasAdvectionTimeError()) {
        viennacore::Logger::getInstance()
            .addError("Advection time integration failed: " +
                          advectionKernel_.getAdvectionTimeError(),
                      false)
            .print();
        return ProcessResult::FAILURE;
      }
    }

    if (context.advectionParams.velocityOutput) {
      auto mesh = viennals::Mesh<NumericType>::New();
      viennals::ToMesh<NumericType, D>(context.domain->getSurface(), mesh)
          .apply();
      viennals::VTKWriter<NumericType>(
          mesh,
          "ls_velocities_" + std::to_string(lsVelOutputCounter_++) + ".vtp")
          .apply();
    }

    context.timeStep = advectionKernel_.getAdvectedTime();

    if (hasExecutor) {
      // Fail-closed path: executor output must be validated before it can
      // advance the shared process state.
      if (context.timeStep == std::numeric_limits<double>::max() ||
          context.timeStep ==
              static_cast<double>(std::numeric_limits<NumericType>::max())) {
        VIENNACORE_LOG_WARNING(
            "Process terminated early: Velocities are zero everywhere.");
        context.processTime = context.processDuration;
        ++totalAdvectionSteps_;
        return ProcessResult::SUCCESS;
      }

      if (!std::isfinite(context.timeStep) || context.timeStep < 0.0) {
        viennacore::Logger::getInstance()
            .addError("Advection produced an invalid time step.", false)
            .print();
        return ProcessResult::FAILURE;
      }

      const double nextProcessTime = context.processTime + context.timeStep;
      if (!std::isfinite(nextProcessTime)) {
        viennacore::Logger::getInstance()
            .addError("Advection produced a non-finite process time.", false)
            .print();
        return ProcessResult::FAILURE;
      }
      if (context.timeStep == 0.0 || !(nextProcessTime > context.processTime)) {
        VIENNACORE_LOG_WARNING(
            "Process terminated early: Advection made no time progress.");
        return ProcessResult::EARLY_TERMINATION;
      }

      context.processTime = nextProcessTime;
      ++totalAdvectionSteps_;
      return ProcessResult::SUCCESS;
    }

    // Legacy CPU path: matches code_reference/ViennaPS 4.6.2 exactly.
    ++totalAdvectionSteps_;
    if (context.timeStep == std::numeric_limits<double>::max()) {
      VIENNACORE_LOG_WARNING(
          "Process terminated early: Velocities are zero everywhere.");
      context.processTime = context.processDuration;
    } else {
      context.processTime += context.timeStep;
    }

    return ProcessResult::SUCCESS;
  }

  ProcessResult copyCoveragesToLevelSet(
      const ProcessContext<NumericType, D> &context,
      SmartPointer<std::unordered_map<unsigned long, unsigned long>> const
          &translator) {
    // Move coverages to the top level set
    auto topLS = context.domain->getSurface();
    auto coverages = context.model->getSurfaceModel()->getCoverages();
    assert(coverages != nullptr);
    assert(translator != nullptr);

    std::vector<std::vector<NumericType>> levelSetCoverages(
        coverages->getScalarDataSize());

#pragma omp parallel for
    for (unsigned i = 0; i < levelSetCoverages.size(); i++) {
      auto covName = coverages->getScalarDataLabel(i);
      std::vector<NumericType> levelSetData(topLS->getNumberOfPoints(), 0);
      auto cov = coverages->getScalarData(covName);

      for (const auto &[lsId, surfaceId] : *translator) {
        levelSetData[lsId] = cov->at(surfaceId);
      }

      levelSetCoverages[i] = std::move(levelSetData);
    }

    for (unsigned i = 0; i < levelSetCoverages.size(); i++) {
      auto covName = coverages->getScalarDataLabel(i);
      topLS->getPointData().insertReplaceScalarData(
          std::move(levelSetCoverages[i]), covName);
    }

    return ProcessResult::SUCCESS;
  }

  ProcessResult updateCoveragesFromAdvectedSurface(
      const ProcessContext<NumericType, D> &context,
      SmartPointer<std::unordered_map<unsigned long, unsigned long>> const
          &translator) {
    // Update coverages from the advected surface
    auto topLS = context.domain->getSurface();
    auto coverages = context.model->getSurfaceModel()->getCoverages();
    assert(coverages != nullptr);
    assert(translator != nullptr);

    for (size_t i = 0; i < coverages->getScalarDataSize(); i++) {
      auto covName = coverages->getScalarDataLabel(i);
      auto levelSetData = topLS->getPointData().getScalarData(covName);
      auto covData = coverages->getScalarData(covName);
      covData->resize(translator->size());

      for (const auto &[lsId, surfaceId] : *translator) {
        covData->at(surfaceId) = levelSetData->at(lsId);
      }
    }

    return ProcessResult::SUCCESS;
  }
  auto &getTimer() const { return timer_; }
  void resetTimer() { timer_.reset(); }
};

} // namespace viennaps
