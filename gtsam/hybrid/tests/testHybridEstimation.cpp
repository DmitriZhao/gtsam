/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    testHybridEstimation.cpp
 * @brief   Unit tests for end-to-end Hybrid Estimation
 * @author  Varun Agrawal
 */

#include <gtsam/discrete/DiscreteBayesNet.h>
#include <gtsam/geometry/Pose2.h>
#include <gtsam/hybrid/HybridBayesNet.h>
#include <gtsam/hybrid/HybridNonlinearFactorGraph.h>
#include <gtsam/hybrid/HybridNonlinearISAM.h>
#include <gtsam/hybrid/HybridSmoother.h>
#include <gtsam/hybrid/MixtureFactor.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/linear/GaussianBayesNet.h>
#include <gtsam/linear/GaussianBayesTree.h>
#include <gtsam/linear/GaussianFactorGraph.h>
#include <gtsam/linear/JacobianFactor.h>
#include <gtsam/linear/NoiseModel.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>

// Include for test suite
#include <CppUnitLite/TestHarness.h>

#include "Switching.h"

using namespace std;
using namespace gtsam;

using symbol_shorthand::X;

Ordering getOrdering(HybridGaussianFactorGraph& factors,
                     const HybridGaussianFactorGraph& newFactors) {
  factors += newFactors;
  // Get all the discrete keys from the factors
  KeySet allDiscrete = factors.discreteKeys();

  // Create KeyVector with continuous keys followed by discrete keys.
  KeyVector newKeysDiscreteLast;
  const KeySet newFactorKeys = newFactors.keys();
  // Insert continuous keys first.
  for (auto& k : newFactorKeys) {
    if (!allDiscrete.exists(k)) {
      newKeysDiscreteLast.push_back(k);
    }
  }

  // Insert discrete keys at the end
  std::copy(allDiscrete.begin(), allDiscrete.end(),
            std::back_inserter(newKeysDiscreteLast));

  const VariableIndex index(factors);

  // Get an ordering where the new keys are eliminated last
  Ordering ordering = Ordering::ColamdConstrainedLast(
      index, KeyVector(newKeysDiscreteLast.begin(), newKeysDiscreteLast.end()),
      true);
  return ordering;
}

TEST(HybridEstimation, Full) {
  size_t K = 3;
  std::vector<double> measurements = {0, 1, 2};
  // Ground truth discrete seq
  std::vector<size_t> discrete_seq = {1, 1, 0};
  // Switching example of robot moving in 1D
  // with given measurements and equal mode priors.
  Switching switching(K, 1.0, 0.1, measurements, "1/1 1/1");
  HybridGaussianFactorGraph graph = switching.linearizedFactorGraph;

  Ordering hybridOrdering;
  hybridOrdering += X(0);
  hybridOrdering += X(1);
  hybridOrdering += X(2);
  hybridOrdering += M(0);
  hybridOrdering += M(1);
  HybridBayesNet::shared_ptr bayesNet =
      graph.eliminateSequential(hybridOrdering);

  EXPECT_LONGS_EQUAL(5, bayesNet->size());
}

/****************************************************************************/
// Test approximate inference with an additional pruning step.
TEST(HybridEstimation, Incremental) {
  size_t K = 15;
  std::vector<double> measurements = {0, 1, 2, 2, 2, 2,  3,  4,  5,  6, 6,
                                      7, 8, 9, 9, 9, 10, 11, 11, 11, 11};
  // Ground truth discrete seq
  std::vector<size_t> discrete_seq = {1, 1, 0, 0, 0, 1, 1, 1, 1, 0,
                                      1, 1, 1, 0, 0, 1, 1, 0, 0, 0};
  // Switching example of robot moving in 1D with given measurements and equal
  // mode priors.
  Switching switching(K, 1.0, 0.1, measurements, "1/1 1/1");
  HybridSmoother smoother;
  HybridNonlinearFactorGraph graph;
  Values initial;

  // Add the X(0) prior
  graph.push_back(switching.nonlinearFactorGraph.at(0));
  initial.insert(X(0), switching.linearizationPoint.at<double>(X(0)));

  HybridGaussianFactorGraph linearized;
  HybridGaussianFactorGraph bayesNet;

  for (size_t k = 1; k < K; k++) {
    // Motion Model
    graph.push_back(switching.nonlinearFactorGraph.at(k));
    // Measurement
    graph.push_back(switching.nonlinearFactorGraph.at(k + K - 1));

    initial.insert(X(k), switching.linearizationPoint.at<double>(X(k)));

    bayesNet = smoother.hybridBayesNet();
    linearized = *graph.linearize(initial);
    Ordering ordering = getOrdering(bayesNet, linearized);

    smoother.update(linearized, ordering, 3);
    graph.resize(0);
  }

  HybridValues delta = smoother.hybridBayesNet().optimize();

  Values result = initial.retract(delta.continuous());

  DiscreteValues expected_discrete;
  for (size_t k = 0; k < K - 1; k++) {
    expected_discrete[M(k)] = discrete_seq[k];
  }
  EXPECT(assert_equal(expected_discrete, delta.discrete()));

  Values expected_continuous;
  for (size_t k = 0; k < K; k++) {
    expected_continuous.insert(X(k), measurements[k]);
  }
  EXPECT(assert_equal(expected_continuous, result));
}

/**
 * @brief A function to get a specific 1D robot motion problem as a linearized
 * factor graph. This is the problem P(X|Z, M), i.e. estimating the continuous
 * positions given the measurements and discrete sequence.
 *
 * @param K The number of timesteps.
 * @param measurements The vector of measurements for each timestep.
 * @param discrete_seq The discrete sequence governing the motion of the robot.
 * @param measurement_sigma Noise model sigma for measurements.
 * @param between_sigma Noise model sigma for the between factor.
 * @return GaussianFactorGraph::shared_ptr
 */
GaussianFactorGraph::shared_ptr specificModesFactorGraph(
    size_t K, const std::vector<double>& measurements,
    const std::vector<size_t>& discrete_seq, double measurement_sigma = 0.1,
    double between_sigma = 1.0) {
  NonlinearFactorGraph graph;
  Values linearizationPoint;

  // Add measurement factors
  auto measurement_noise = noiseModel::Isotropic::Sigma(1, measurement_sigma);
  for (size_t k = 0; k < K; k++) {
    graph.emplace_shared<PriorFactor<double>>(X(k), measurements.at(k),
                                              measurement_noise);
    linearizationPoint.insert<double>(X(k), static_cast<double>(k + 1));
  }

  using MotionModel = BetweenFactor<double>;

  // Add "motion models".
  auto motion_noise_model = noiseModel::Isotropic::Sigma(1, between_sigma);
  for (size_t k = 0; k < K - 1; k++) {
    auto motion_model = boost::make_shared<MotionModel>(
        X(k), X(k + 1), discrete_seq.at(k), motion_noise_model);
    graph.push_back(motion_model);
  }
  GaussianFactorGraph::shared_ptr linear_graph =
      graph.linearize(linearizationPoint);
  return linear_graph;
}

/**
 * @brief Get the discrete sequence from the integer `x`.
 *
 * @tparam K Template parameter so we can set the correct bitset size.
 * @param x The integer to convert to a discrete binary sequence.
 * @return std::vector<size_t>
 */
template <size_t K>
std::vector<size_t> getDiscreteSequence(size_t x) {
  std::bitset<K - 1> seq = x;
  std::vector<size_t> discrete_seq(K - 1);
  for (size_t i = 0; i < K - 1; i++) {
    // Save to discrete vector in reverse order
    discrete_seq[K - 2 - i] = seq[i];
  }
  return discrete_seq;
}

/**
 * @brief Helper method to get the tree of unnormalized probabilities
 * as per the new elimination scheme.
 *
 * @param graph The HybridGaussianFactorGraph to eliminate.
 * @return AlgebraicDecisionTree<Key>
 */
AlgebraicDecisionTree<Key> probPrimeTree(
    const HybridGaussianFactorGraph& graph) {
  HybridBayesNet::shared_ptr bayesNet;
  HybridGaussianFactorGraph::shared_ptr remainingGraph;
  Ordering continuous(graph.continuousKeys());
  std::tie(bayesNet, remainingGraph) =
      graph.eliminatePartialSequential(continuous);

  auto last_conditional = bayesNet->at(bayesNet->size() - 1);
  DiscreteKeys discrete_keys = last_conditional->discreteKeys();

  const std::vector<DiscreteValues> assignments =
      DiscreteValues::CartesianProduct(discrete_keys);

  std::reverse(discrete_keys.begin(), discrete_keys.end());

  vector<VectorValues::shared_ptr> vector_values;
  for (const DiscreteValues& assignment : assignments) {
    VectorValues values = bayesNet->optimize(assignment);
    vector_values.push_back(boost::make_shared<VectorValues>(values));
  }
  DecisionTree<Key, VectorValues::shared_ptr> delta_tree(discrete_keys,
                                                         vector_values);

  std::vector<double> probPrimes;
  for (const DiscreteValues& assignment : assignments) {
    double error = 0.0;
    VectorValues delta = *delta_tree(assignment);
    for (auto factor : graph) {
      if (factor->isHybrid()) {
        auto f = boost::static_pointer_cast<GaussianMixtureFactor>(factor);
        error += f->error(delta, assignment);

      } else if (factor->isContinuous()) {
        auto f = boost::static_pointer_cast<HybridGaussianFactor>(factor);
        error += f->inner()->error(delta);
      }
    }
    probPrimes.push_back(exp(-error));
  }
  AlgebraicDecisionTree<Key> probPrimeTree(discrete_keys, probPrimes);
  return probPrimeTree;
}

/****************************************************************************/
/**
 * Test for correctness of different branches of the P'(Continuous | Discrete).
 * The values should match those of P'(Continuous) for each discrete mode.
 */
TEST(HybridEstimation, Probability) {
  constexpr size_t K = 4;
  std::vector<double> measurements = {0, 1, 2, 2};
  double between_sigma = 1.0, measurement_sigma = 0.1;

  // Switching example of robot moving in 1D with
  // given measurements and equal mode priors.
  Switching switching(K, between_sigma, measurement_sigma, measurements,
                      "1/1 1/1");
  auto graph = switching.linearizedFactorGraph;
  Ordering ordering = getOrdering(graph, HybridGaussianFactorGraph());

  HybridBayesNet::shared_ptr bayesNet = graph.eliminateSequential(ordering);
  auto discreteConditional = bayesNet->atDiscrete(bayesNet->size() - 3);

  HybridValues hybrid_values = bayesNet->optimize();

  // This is the correct sequence as designed
  DiscreteValues discrete_seq;
  discrete_seq[M(0)] = 1;
  discrete_seq[M(1)] = 1;
  discrete_seq[M(2)] = 0;

  EXPECT(assert_equal(discrete_seq, hybrid_values.discrete()));
}

/****************************************************************************/
/**
 * Test for correctness of different branches of the P'(Continuous | Discrete)
 * in the multi-frontal setting. The values should match those of P'(Continuous)
 * for each discrete mode.
 */
TEST(HybridEstimation, ProbabilityMultifrontal) {
  constexpr size_t K = 4;
  std::vector<double> measurements = {0, 1, 2, 2};

  double between_sigma = 1.0, measurement_sigma = 0.1;

  // Switching example of robot moving in 1D with given measurements and equal
  // mode priors.
  Switching switching(K, between_sigma, measurement_sigma, measurements,
                      "1/1 1/1");
  auto graph = switching.linearizedFactorGraph;
  Ordering ordering = getOrdering(graph, HybridGaussianFactorGraph());

  // Get the tree of unnormalized probabilities for each mode sequence.
  AlgebraicDecisionTree<Key> expected_probPrimeTree = probPrimeTree(graph);

  // Eliminate continuous
  Ordering continuous_ordering(graph.continuousKeys());
  HybridBayesTree::shared_ptr bayesTree;
  HybridGaussianFactorGraph::shared_ptr discreteGraph;
  std::tie(bayesTree, discreteGraph) =
      graph.eliminatePartialMultifrontal(continuous_ordering);

  // Get the last continuous conditional which will have all the discrete keys
  Key last_continuous_key =
      continuous_ordering.at(continuous_ordering.size() - 1);
  auto last_conditional = (*bayesTree)[last_continuous_key]->conditional();
  DiscreteKeys discrete_keys = last_conditional->discreteKeys();

  Ordering discrete(graph.discreteKeys());
  auto discreteBayesTree =
      discreteGraph->BaseEliminateable::eliminateMultifrontal(discrete);

  EXPECT_LONGS_EQUAL(1, discreteBayesTree->size());
  // DiscreteBayesTree should have only 1 clique
  auto discrete_clique = (*discreteBayesTree)[discrete.at(0)];

  std::set<HybridBayesTreeClique::shared_ptr> clique_set;
  for (auto node : bayesTree->nodes()) {
    clique_set.insert(node.second);
  }

  // Set the root of the bayes tree as the discrete clique
  for (auto clique : clique_set) {
    if (clique->conditional()->parents() ==
        discrete_clique->conditional()->frontals()) {
      discreteBayesTree->addClique(clique, discrete_clique);

    } else {
      // Remove the clique from the children of the parents since it will get
      // added again in addClique.
      auto clique_it = std::find(clique->parent()->children.begin(),
                                 clique->parent()->children.end(), clique);
      clique->parent()->children.erase(clique_it);
      discreteBayesTree->addClique(clique, clique->parent());
    }
  }

  HybridValues hybrid_values = discreteBayesTree->optimize();

  // This is the correct sequence as designed
  DiscreteValues discrete_seq;
  discrete_seq[M(0)] = 1;
  discrete_seq[M(1)] = 1;
  discrete_seq[M(2)] = 0;

  EXPECT(assert_equal(discrete_seq, hybrid_values.discrete()));
}

/*********************************************************************************
  // Create a hybrid nonlinear factor graph f(x0, x1, m0; z0, z1)
 ********************************************************************************/
static HybridNonlinearFactorGraph createHybridNonlinearFactorGraph() {
  HybridNonlinearFactorGraph nfg;

  constexpr double sigma = 0.5;  // measurement noise
  const auto noise_model = noiseModel::Isotropic::Sigma(1, sigma);

  // Add "measurement" factors:
  nfg.emplace_nonlinear<PriorFactor<double>>(X(0), 0.0, noise_model);
  nfg.emplace_nonlinear<PriorFactor<double>>(X(1), 1.0, noise_model);

  // Add mixture factor:
  DiscreteKey m(M(0), 2);
  const auto zero_motion =
      boost::make_shared<BetweenFactor<double>>(X(0), X(1), 0, noise_model);
  const auto one_motion =
      boost::make_shared<BetweenFactor<double>>(X(0), X(1), 1, noise_model);
  nfg.emplace_hybrid<MixtureFactor>(
      KeyVector{X(0), X(1)}, DiscreteKeys{m},
      std::vector<NonlinearFactor::shared_ptr>{zero_motion, one_motion});

  return nfg;
}

/*********************************************************************************
  // Create a hybrid nonlinear factor graph f(x0, x1, m0; z0, z1)
 ********************************************************************************/
static HybridGaussianFactorGraph::shared_ptr createHybridGaussianFactorGraph() {
  HybridNonlinearFactorGraph nfg = createHybridNonlinearFactorGraph();

  Values initial;
  double z0 = 0.0, z1 = 1.0;
  initial.insert<double>(X(0), z0);
  initial.insert<double>(X(1), z1);
  return nfg.linearize(initial);
}

/*********************************************************************************
 * Do hybrid elimination and do regression test on discrete conditional.
 ********************************************************************************/
TEST(HybridEstimation, eliminateSequentialRegression) {
  // 1. Create the factor graph from the nonlinear factor graph.
  HybridGaussianFactorGraph::shared_ptr fg = createHybridGaussianFactorGraph();

  // 2. Eliminate into BN
  const Ordering ordering = fg->getHybridOrdering();
  HybridBayesNet::shared_ptr bn = fg->eliminateSequential(ordering);
  // GTSAM_PRINT(*bn);

  // TODO(dellaert): dc should be discrete conditional on m0, but it is an
  // unnormalized factor? DiscreteKey m(M(0), 2); DiscreteConditional expected(m
  // % "0.51341712/1"); auto dc = bn->back()->asDiscreteConditional();
  // EXPECT(assert_equal(expected, *dc, 1e-9));
}

/*********************************************************************************
 * Test for correctness via sampling.
 *
 * Compute the conditional P(x0, m0, x1| z0, z1)
 * with measurements z0, z1. To do so, we:
 * 1. Start with the corresponding Factor Graph `FG`.
 * 2. Eliminate the factor graph into a Bayes Net `BN`.
 * 3. Sample from the Bayes Net.
 * 4. Check that the ratio `BN(x)/FG(x) = constant` for all samples `x`.
 ********************************************************************************/
TEST(HybridEstimation, CorrectnessViaSampling) {
  // 1. Create the factor graph from the nonlinear factor graph.
  HybridGaussianFactorGraph::shared_ptr fg = createHybridGaussianFactorGraph();

  // 2. Eliminate into BN
  const Ordering ordering = fg->getHybridOrdering();
  HybridBayesNet::shared_ptr bn = fg->eliminateSequential(ordering);

  // Set up sampling
  std::mt19937_64 rng(11);

  // 3. Do sampling
  int num_samples = 10;

  // Functor to compute the ratio between the
  // Bayes net and the factor graph.
  auto compute_ratio =
      [](const HybridBayesNet::shared_ptr& bayesNet,
         const HybridGaussianFactorGraph::shared_ptr& factorGraph,
         const HybridValues& sample) -> double {
    const DiscreteValues assignment = sample.discrete();
    // Compute in log form for numerical stability
    double log_ratio = bayesNet->error(sample.continuous(), assignment) -
                       factorGraph->error(sample.continuous(), assignment);
    double ratio = exp(-log_ratio);
    return ratio;
  };

  // The error evaluated by the factor graph and the Bayes net should differ by
  // the normalizing term computed via the Bayes net determinant.
  const HybridValues sample = bn->sample(&rng);
  double ratio = compute_ratio(bn, fg, sample);
  // regression
  EXPECT_DOUBLES_EQUAL(1.0, ratio, 1e-9);

  // 4. Check that all samples == constant
  for (size_t i = 0; i < num_samples; i++) {
    // Sample from the bayes net
    const HybridValues sample = bn->sample(&rng);

    EXPECT_DOUBLES_EQUAL(ratio, compute_ratio(bn, fg, sample), 1e-9);
  }
}

/* ************************************************************************* */
int main() {
  TestResult tr;
  return TestRegistry::runAllTests(tr);
}
/* ************************************************************************* */
