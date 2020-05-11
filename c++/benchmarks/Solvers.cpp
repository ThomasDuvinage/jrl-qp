/* Copyright 2020 CNRS-AIST JRL
 */

#include <array>
#include <iostream>
#include <map>
#include <set>

#include <benchmark/benchmark.h>

#include <eigen-lssol/LSSOL_QP.h>
#include <eigen-quadprog/QuadProg.h>
#include <eigen-qld/QLDDirect.h>

#include <jrl-qp/GoldfarbIdnaniSolver.h>
#include <jrl-qp/test/problems.h>
#include <jrl-qp/test/randomProblems.h>

// To be found at 
// https://bitbucket.ihmc.us/projects/LIBS/repos/ihmc-open-robotics-software/browse/ihmc-common-walking-control-modules/csrc/ActiveSetQP/eiquadprog.hpp
// and copied in the benchmarks directory (for now) [TODO] make this proper.
#include "eiquadprog.hpp"
#include "problemAdaptors.h"

using namespace Eigen;
using namespace jrlqp;
using namespace jrlqp::test;

enum class ParamType
{
  Variable,
  Fixed,
  FixedFraction,
  VariableFraction
};

template<int i> 
struct Var
{
  constexpr static int rangeIdx = i;
  constexpr static ParamType type = ParamType::Variable;
  constexpr static int rangeSlot = 1;
  static int value(const ::benchmark::State& st, int) { return st.range(i); }
};

template<int i>
struct Fixed
{
  constexpr static ParamType type = ParamType::Fixed;
  constexpr static int rangeSlot = 0;
  static int value(const ::benchmark::State&, int) { return i; }
};

template<int n, int d=100>
struct FFrac
{
  constexpr static double frac = static_cast<double>(n)/d;
  constexpr static ParamType type = ParamType::FixedFraction;
  constexpr static int rangeSlot = 0;
  static int value(const ::benchmark::State&, double ref) { return static_cast<int>(frac * ref); }
};

template<int i, int d=100>
struct VFrac
{
  constexpr static double invd = 1. / d;
  constexpr static ParamType type = ParamType::VariableFraction;
  constexpr static int rangeIdx = i;
  constexpr static int rangeSlot = 1;
  static int value(const ::benchmark::State& st, double ref) { return static_cast<int>(st.range(i) * ref * invd); }
};

template<bool bounds, bool doubleSided>
constexpr int packBool()
{
  int r;
  if (bounds) r = 1; else r = 0;
  if (doubleSided) r += 2;
  return r;
}

template<typename NVar, typename NEq, typename NIneq, typename NIneqAct, typename NBndAct>
constexpr int rangeSize()
{
  return 1 + NVar::rangeSlot + NEq::rangeSlot + NIneq::rangeSlot + NIneqAct::rangeSlot + NBndAct::rangeSlot;
}

template<typename NVar, typename NEq, typename NIneq, typename NIneqAct, typename NBndAct>
using SignatureType = std::array<int, rangeSize<NVar, NEq, NIneq, NIneqAct, NBndAct>()>;

template<typename NVar, typename NEq, typename NIneq, typename NIneqAct, bool Bounds, typename NBndAct, bool DoubleSided = false>
SignatureType<NVar, NEq, NIneq, NIneqAct, NBndAct> problemSignature(const ::benchmark::State& st)
{
  SignatureType<NVar, NEq, NIneq, NIneqAct, NBndAct> ret;
  ret[0] = packBool<Bounds, DoubleSided>();
  if constexpr (NVar::rangeSlot) ret[NVar::rangeIdx + 1] = st.range(NVar::rangeIdx);
  if constexpr (NEq::rangeSlot) ret[NEq::rangeIdx + 1] = st.range(NEq::rangeIdx);
  if constexpr (NIneq::rangeSlot) ret[NIneq::rangeIdx + 1] = st.range(NIneq::rangeIdx);
  if constexpr (NIneqAct::rangeSlot) ret[NIneqAct::rangeIdx + 1] = st.range(NIneqAct::rangeIdx);
  if constexpr (NBndAct::rangeSlot) ret[NBndAct::rangeIdx + 1] = st.range(NBndAct::rangeIdx);

  return ret;
}

template<int NbPb>
struct ProblemCollection
{
  void generate(int n, int me, int mi, int ma, int na, bool bounds, bool doubleSided)
  {
    for (int k = 0; k < NbPb; ++k)
    {
      original[k] = randomProblem(ProblemCharacteristics(n, n, me, mi)
                                  .nStrongActIneq(ma)
                                  .nStrongActBounds(na)
                                  .bounds(bounds)
                                  .doubleSidedIneq(doubleSided));
      QPProblem<true> qp = original[k];
      giPb[k] = qp;
      G[k] = giPb[k].G;
      lssolPb[k] = qp;
      quadprogPb[k] = qp;
      eiquadprogPb[k] = qp;
      qldPb[k] = qp;
    }
    nVar = n;
    nEq = me;
    nIneq = mi;
    nSSIneqAndBnd = quadprogPb[0].Aineq.rows();
    nCstr = me + mi;
    this->bounds = bounds;
    this->doubleSided = doubleSided;
  }

  void check()
  {
    Eigen::VectorXd x(nVar);
    GoldfarbIdnaniSolver solverGI(nVar, nCstr, bounds);
    Eigen::QuadProgDense solverQP(nVar, nEq, nSSIneqAndBnd);
    Eigen::LSSOL_QP solverLS(nVar, nCstr, Eigen::lssol::QP2);
    solverLS.optimalityMaxIter(500);
    solverLS.feasibilityMaxIter(500);
    //Eigen::QLDDirect solverQLD(nVar, nEq, nIneq);
    for (int k = 0; k < NbPb; ++k)
    {
      {
        auto& qp = giPb[k];
        solverGI.solve(qp.G, qp.a, qp.C, qp.l, qp.u, qp.xl, qp.xu);
        checkSolution(solverGI.solution(), k, "GI");
      }
      {
        auto& qp = eiquadprogPb[k];
        Eigen::solve_quadprog(qp.G, qp.g0, qp.CE, qp.ce0, qp.CI, qp.ci0, x);
        checkSolution(x, k, "eiQuadprog");
      }
      {
        auto& qp = quadprogPb[k];
        solverQP.solve(qp.Q, qp.c, qp.Aeq, qp.beq, qp.Aineq, qp.bineq);
        checkSolution(solverQP.result(), k, "quadprog");
      }
      {
        auto& qp = lssolPb[k];
        solverLS.solve(qp.Q, qp.p, qp.C, qp.l, qp.u);
        checkSolution(solverLS.result(), k, "lssol");
      }
      //{
      //  auto& qp = qldPb[k];
      //  solverQLD.solve(qp.Q, qp.c, qp.A, qp.b, qp.xl, qp.xu, nEq);
      //  checkSolution(solverQLD.result(), k, "qld");
      //}
    }
  }

  void checkSolution(const VectorConstRef& x, int k, const std::string& name)
  {
    double err = (x - original[k].x).norm();
    if (err > 1e-6)
      throw std::runtime_error("Unexpected solution for " + name);
  }

  int nVar;
  int nEq;
  int nIneq;
  int nSSIneqAndBnd;
  int nCstr;
  bool bounds;
  bool doubleSided;
  std::array<RandomLeastSquare, NbPb> original;
  std::array<MatrixXd, NbPb> G;
  std::array<GIPb, NbPb> giPb;
  std::array<LssolPb, NbPb> lssolPb;
  std::array<EigenQuadprogPb, NbPb> quadprogPb;
  std::array<EiQuadprogPb, NbPb> eiquadprogPb;
  std::array<QLDPb, NbPb> qldPb;
};


template<int NbPb, typename NVar, typename NEq, typename NIneq, typename NIneqAct, bool Bounds, typename NBndAct, bool DoubleSided = false>
class ProblemFixture : public ::benchmark::Fixture
{
public:
  using Signature = SignatureType<NVar, NEq, NIneq, NIneqAct, NBndAct>;

  void SetUp(const ::benchmark::State& st)
  {
    static std::set<Signature> initialized = {};

    auto sig = problemSignature<NVar, NEq, NIneq, NIneqAct, Bounds, NBndAct, DoubleSided>(st);

    i = 0;
    if (initialized.find(sig) == initialized.end())
    {
      int n = NVar::value(st, 0);
      int me = NEq::value(st, n);
      int mi = NIneq::value(st, n);
      int ma = NIneqAct::value(st, std::min(n, mi));
      int na = NBndAct::value(st, n);

      problems[sig] = {};

      int nTry;
      for (nTry = 0; nTry < 5; ++nTry)
      {
        std::cout << "initialize for (" << n << ", " << me << ", " << mi << ", " << ma << ", " << na << ", " << Bounds << ", " << DoubleSided << ")" << std::endl;
        problems[sig].generate(n, me, mi, ma, na, Bounds, DoubleSided);
        try
        {
          problems[sig].check();
          break;
        }
        catch (std::runtime_error e)
        {
          std::cout << e.what() << std::endl;
          std::cout << "retry" << std::endl;
        }
      }
      if (nTry >= 5)
        throw std::runtime_error("unable to generate problems");
      initialized.insert(sig);
    }
    
  }

  void TearDown(const ::benchmark::State&)
  {
  }

  Signature signature(const ::benchmark::State& st)
  {
    return problemSignature<NVar, NEq, NIneq, NIneqAct, Bounds, NBndAct, DoubleSided>(st);
  }

  int idx()
  {
    int ret = i % NbPb;
    ++i;
    return ret;
  }

  int nVar(const Signature& sig) const { return problems[sig].nVar; }
  int nEq(const Signature& sig) const { return problems[sig].nEq; }
  int nIneq(const Signature& sig) const { return problems[sig].nIneq; }
  // Number of single-sided constraints includingBounds
  int nSSIneqAndBnd(const Signature& sig) const { return problems[sig].nSSIneqAndBnd; }
  int nCstr(const Signature& sig) const { return problems[sig].nCstr; }
  int bounds(const Signature& sig) const { return problems[sig].bounds; }

  RandomLeastSquare& getOriginal() { return problems[this->sig].original[idx()]; }

  GIPb& getGIPb(const Signature& sig) 
  { 
    int i = idx();
    auto& pb = problems[sig];
    pb.giPb[i].G = pb.G[i];  
    return pb.giPb[i];
  }

  LssolPb& getLssolPb(const Signature& sig)
  {
    int i = idx();
    auto& pb = problems[sig];
    pb.lssolPb[i].Q = pb.G[i];
    return pb.lssolPb[i];
  }

  EigenQuadprogPb& getQuadprogPb(const Signature& sig)
  {
    int i = idx();
    auto& pb = problems[sig];
    pb.quadprogPb[i].Q = pb.G[i];
    return pb.quadprogPb[i];
  }

  EiQuadprogPb& getEiQuadprogPb(const Signature& sig)
  {
    int i = idx();
    auto& pb = problems[sig];
    pb.eiquadprogPb[i].G = pb.G[i];
    return pb.eiquadprogPb[i];
  }

  QLDPb& getQLDPb(const Signature& sig)
  {
    int i = idx();
    auto& pb = problems[sig];
    pb.qldPb[i].Q = pb.G[i];
    return pb.qldPb[i];
  }

private:
  int i;
  
  inline static std::map<Signature,ProblemCollection<NbPb>> problems;
};

#include<iostream>

#define BENCH_OVERHEAD(fixture)                                       \
BENCHMARK_DEFINE_F(fixture, Overhead)(benchmark::State& st)           \
{                                                                     \
  auto sig = signature(st);                                           \
  for (auto _ : st)                                                   \
  {                                                                   \
    benchmark::DoNotOptimize(getGIPb(sig));                           \
  }                                                                   \
}                                                                     \
BENCHMARK_REGISTER_F(fixture, Overhead)->Unit(benchmark::kMicrosecond)

#define BENCH_GI(fixture)                                             \
BENCHMARK_DEFINE_F(fixture, GI)(benchmark::State& st)                 \
{                                                                     \
  auto sig = signature(st);                                           \
  GoldfarbIdnaniSolver solver(nVar(sig), nCstr(sig), bounds(sig));    \
  for (auto _ : st)                                                   \
  {                                                                   \
    auto& qp = getGIPb(sig);                                          \
    solver.solve(qp.G, qp.a, qp.C, qp.l, qp.u, qp.xl, qp.xu);         \
  }                                                                   \
}                                                                     \
BENCHMARK_REGISTER_F(fixture, GI)->Unit(benchmark::kMicrosecond)

#define BENCH_EIQP(fixture)                                               \
BENCHMARK_DEFINE_F(fixture, EIQP)(benchmark::State& st)                   \
{                                                                         \
  auto sig = signature(st);                                               \
  Eigen::VectorXd x(nVar(sig));                                           \
  for (auto _ : st)                                                       \
  {                                                                       \
    auto& qp = getEiQuadprogPb(sig);                                      \
    Eigen::solve_quadprog(qp.G, qp.g0, qp.CE, qp.ce0, qp.CI, qp.ci0, x);  \
  }                                                                       \
}                                                                         \
BENCHMARK_REGISTER_F(fixture, EIQP)->Unit(benchmark::kMicrosecond)

#define BENCH_QUADPROG(fixture)                                       \
BENCHMARK_DEFINE_F(fixture, QuadProg)(benchmark::State& st)           \
{                                                                     \
  auto sig = signature(st);                                           \
  Eigen::QuadProgDense solver(nVar(sig),nEq(sig),nSSIneqAndBnd(sig)); \
                                                                      \
  for (auto _ : st)                                                   \
  {                                                                   \
    auto& qp = getQuadprogPb(sig);                                    \
    solver.solve(qp.Q, qp.c, qp.Aeq, qp.beq, qp.Aineq, qp.bineq);     \
  }                                                                   \
}                                                                     \
BENCHMARK_REGISTER_F(fixture, QuadProg)->Unit(benchmark::kMicrosecond)

#define BENCH_LSSOL(fixture)                                        \
BENCHMARK_DEFINE_F(fixture, Lssol)(benchmark::State& st)            \
{                                                                   \
  auto sig = signature(st);                                         \
  Eigen::LSSOL_QP solver(nVar(sig), nCstr(sig), Eigen::lssol::QP2); \
  solver.optimalityMaxIter(500);                                    \
  solver.feasibilityMaxIter(500);                                   \
  for (auto _ : st)                                                 \
  {                                                                 \
    auto& qp = getLssolPb(sig);                                     \
    solver.solve(qp.Q, qp.p, qp.C, qp.l, qp.u);                     \
  }                                                                 \
}                                                                   \
BENCHMARK_REGISTER_F(fixture, Lssol)->Unit(benchmark::kMicrosecond)

//BENCHMARK_DEFINE_F(test1, LssolHackyWarmstart)(benchmark::State& st)
//{
//  Eigen::LSSOL_QP solver(nVar(), nCstr(), Eigen::lssol::QP2);
//  solver.optimalityMaxIter(500);
//  solver.feasibilityMaxIter(500);
//  solver.warm(true);
//  solver.persistence(true);
//  auto& qp = getLssolPb();
//  MatrixXd Q = qp.Q;
//  for (auto _ : st)
//  {
//    qp.Q = Q;
//    solver.solve(qp.Q, qp.p, qp.C, qp.l, qp.u);
//  }
//}
//BENCHMARK_REGISTER_F(test1, LssolHackyWarmstart);

//BENCHMARK_DEFINE_F(test1, QLD)(benchmark::State& st)
//{
//  Eigen::QLDDirect solver(100, 40, 100);
//  for (auto _ : st)
//  {
//    auto& qp = getQLDPb();
//    solver.solve(qp.Q, qp.c, qp.A, qp.b, qp.xl, qp.xu, 40);
//  }
//}
//BENCHMARK_REGISTER_F(test1, QLD);


#define BENCH_ALL(fixture, otherArgs) \
BENCH_OVERHEAD(fixture)otherArgs;       \
BENCH_GI(fixture)otherArgs;             \
BENCH_EIQP(fixture)otherArgs;           \
BENCH_QUADPROG(fixture)otherArgs;       \
BENCH_LSSOL(fixture)otherArgs;          \

//using test1 = ProblemFixture<100, Fixed<100>, Fixed<40>, Fixed<100>, Fixed<40>, false, Fixed<0>>;
//using test1 = ProblemFixture<100, Fixed<100>, Fixed<0>, Fixed<50>, Fixed<0>, false, Fixed<0>>;

// Varying size, fixed 40% equality
using test1 = ProblemFixture<100, Var<0>, FFrac<40>, Fixed<0>, Fixed<0>, false, Fixed<0>>;
BENCH_ALL(test1, ->DenseRange(10,100,10));

//Fixed nVar = 50 and nIneq=80, varying number of active constraints from 0 to 100%
using test2 = ProblemFixture<100, Fixed<50>, Fixed<0>, Fixed<80>, VFrac<0>, false, Fixed<0>>;
BENCH_ALL(test2, ->DenseRange(0, 100, 10));

// Varying size, fixed 20% equality, fixed 100% inequality, with 30% active, bounds 
using test3 = ProblemFixture<100, Var<0>, FFrac<20>, FFrac<100>, FFrac<30>, true, FFrac<10>, true>;
BENCH_ALL(test3, ->DenseRange(10,100,10));

BENCHMARK_MAIN();
