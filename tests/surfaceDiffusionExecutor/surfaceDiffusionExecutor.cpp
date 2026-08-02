#include <process/psProcess.hpp>

#include <vcTestAsserts.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

template <typename NumericType>
viennaps::ProcessResult publishCandidate(
    viennaps::ProcessResult result,
    const viennaps::SurfaceDiffusionWork<NumericType> &work,
    const std::vector<NumericType> &candidate,
    std::vector<NumericType> &current) {
  if (result != viennaps::ProcessResult::SUCCESS)
    return result;
  if (!work.complete || work.writtenCount != candidate.size())
    return viennaps::ProcessResult::FAILURE;
  current = candidate;
  return viennaps::ProcessResult::SUCCESS;
}

template <typename NumericType>
bool runFacadeSmoke() {
  using Process = viennaps::Process<NumericType, 2>;
  Process process;
  VC_TEST_ASSERT(!process.getSurfaceDiffusionExecutor());
  unsigned calls = 0U;
  typename Process::SurfaceDiffusionExecutor executor =
      [&calls](viennaps::SurfaceDiffusionWork<NumericType> &work,
               std::string &error) {
        ++calls;
        if (work.rowOffsets.size() != 3U ||
            work.columnIndices.size() != 3U || work.weights.size() != 3U ||
            work.field.size() != 2U || work.output.size() != 2U) {
          error = "unexpected surface diffusion work dimensions";
          return viennaps::ProcessResult::FAILURE;
        }
        work.output[0] = work.field[0] + work.diffusionStep;
        work.output[1] = work.field[1] - work.diffusionStep;
        work.writtenCount = work.output.size();
        work.complete = true;
        return viennaps::ProcessResult::SUCCESS;
      };

  process.setSurfaceDiffusionExecutor(executor);
  VC_TEST_ASSERT(static_cast<bool>(process.getSurfaceDiffusionExecutor()));

  const std::vector<std::uint32_t> rowOffsets{0U, 2U, 3U};
  const std::vector<std::uint32_t> columnIndices{0U, 1U, 1U};
  const std::vector<NumericType> weights{NumericType(1), NumericType(-1),
                                         NumericType(2)};
  const std::vector<NumericType> field{NumericType(3), NumericType(4)};
  std::vector<NumericType> output(2U, NumericType(0));
  viennaps::SurfaceDiffusionWork<NumericType> work{
      rowOffsets, columnIndices, weights, field, output, NumericType(0.5), 0U,
      false};
  std::string error;
  const auto result = process.getSurfaceDiffusionExecutor()(work, error);
  std::vector<NumericType> current = field;
  VC_TEST_ASSERT(publishCandidate(result, work, output, current) ==
                 viennaps::ProcessResult::SUCCESS);
  VC_TEST_ASSERT(calls == 1U);
  VC_TEST_ASSERT(output[0] == NumericType(3.5));
  VC_TEST_ASSERT(output[1] == NumericType(3.5));
  VC_TEST_ASSERT(current == output);

  if constexpr (std::is_same_v<NumericType, float>) {
    unsigned statusCalls = 0U;
    typename Process::SurfaceDiffusionStatusExecutor statusExecutor =
        [&statusCalls](viennaps::SurfaceDiffusionWork<float> &statusWork,
                       std::string &) {
          ++statusCalls;
          statusWork.output[0] = statusWork.field[0] + 1.0F;
          statusWork.output[1] = statusWork.field[1] + 1.0F;
          statusWork.writtenCount = statusWork.output.size();
          statusWork.complete = true;
          return viennaps::SurfaceDiffusionExecutionStatus::SUCCESS;
        };
    process.setSurfaceDiffusionStatusExecutor(statusExecutor);
    std::vector<float> statusOutput(2U, 0.0F);
    viennaps::SurfaceDiffusionWork<float> statusWork{
        rowOffsets, columnIndices, weights, field, statusOutput, 0.5F, 0U,
        false};
    VC_TEST_ASSERT(process.getSurfaceDiffusionExecutor()(statusWork, error) ==
                   viennaps::ProcessResult::SUCCESS);
    VC_TEST_ASSERT(statusCalls == 1U);
    VC_TEST_ASSERT(statusOutput[0] == 4.0F && statusOutput[1] == 5.0F);

    statusExecutor =
        [](viennaps::SurfaceDiffusionWork<float> &, std::string &) {
          return viennaps::SurfaceDiffusionExecutionStatus::FAILURE;
        };
    process.setSurfaceDiffusionStatusExecutor(statusExecutor);
    VC_TEST_ASSERT(process.getSurfaceDiffusionExecutor()(statusWork, error) ==
                   viennaps::ProcessResult::FAILURE);
    process.setSurfaceDiffusionStatusExecutor({});
    VC_TEST_ASSERT(!process.getSurfaceDiffusionExecutor());
  }

  const auto previous = current;
  std::vector<NumericType> partialOutput(2U, NumericType(0));
  viennaps::SurfaceDiffusionWork<NumericType> partialWork{
      rowOffsets, columnIndices, weights, current, partialOutput,
      NumericType(0.5), 1U, false};
  partialOutput[0] = NumericType(99);
  VC_TEST_ASSERT(publishCandidate(viennaps::ProcessResult::SUCCESS,
                                  partialWork, partialOutput, current) ==
                 viennaps::ProcessResult::FAILURE);
  VC_TEST_ASSERT(current == previous);

  std::vector<NumericType> incompleteOutput(2U, NumericType(0));
  viennaps::SurfaceDiffusionWork<NumericType> incompleteWork{
      rowOffsets, columnIndices, weights, current, incompleteOutput,
      NumericType(0.5), incompleteOutput.size(), false};
  VC_TEST_ASSERT(publishCandidate(viennaps::ProcessResult::SUCCESS,
                                  incompleteWork, incompleteOutput, current) ==
                 viennaps::ProcessResult::FAILURE);
  VC_TEST_ASSERT(current == previous);

  typename Process::SurfaceDiffusionExecutor throwingExecutor =
      [](viennaps::SurfaceDiffusionWork<NumericType> &, std::string &) {
        throw std::runtime_error("surface diffusion test exception");
        return viennaps::ProcessResult::FAILURE;
      };
  try {
    throwingExecutor(work, error);
    VC_TEST_ASSERT(false);
  } catch (const std::runtime_error &) {
    VC_TEST_ASSERT(current == previous);
  }

  process.clearSurfaceDiffusionExecutor();
  VC_TEST_ASSERT(!process.getSurfaceDiffusionExecutor());
  return true;
}

} // namespace

int main() {
  return runFacadeSmoke<float>() && runFacadeSmoke<double>() ? 0 : 1;
}
