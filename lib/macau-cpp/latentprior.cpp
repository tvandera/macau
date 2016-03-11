#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <cblas.h>
#include <math.h>
#include <omp.h>

#include "mvnormal.h"
#include "macau.h"
#include "chol.h"
#include "linop.h"
extern "C" {
  #include <sparse.h>
}

using namespace std; 
using namespace Eigen;

/** BPMFPrior */
void BPMFPrior::sample_latents(Eigen::MatrixXd &U, const Eigen::SparseMatrix<double> &mat, double mean_value,
                    const Eigen::MatrixXd &samples, double alpha, const int num_latent) {
  const int N = U.cols();
  
#pragma omp parallel for
  for(int n = 0; n < N; n++) {
    sample_latent_blas(U, n, mat, mean_value, samples, alpha, mu, Lambda, num_latent);
  }
}

void BPMFPrior::update_prior(const Eigen::MatrixXd &U) {
  tie(mu, Lambda) = CondNormalWishart(U, mu0, b0, WI, df);
}


void BPMFPrior::init(const int num_latent) {
  mu.resize(num_latent);
  mu.setZero();

  Lambda.resize(num_latent, num_latent);
  Lambda.setIdentity();
  Lambda *= 10;

  // parameters of Inv-Whishart distribution
  WI.resize(num_latent, num_latent);
  WI.setIdentity();
  mu0.resize(num_latent);
  mu0.setZero();
  b0 = 2;
  df = num_latent;
}

/** MacauPrior */
template<class FType>
void MacauPrior<FType>::init(const int num_latent, FType & Fmat, bool comp_FtF) {
  mu.resize(num_latent);
  mu.setZero();

  Lambda.resize(num_latent, num_latent);
  Lambda.setIdentity();
  Lambda *= 10;

  // parameters of Inv-Whishart distribution
  WI.resize(num_latent, num_latent);
  WI.setIdentity();
  mu0.resize(num_latent);
  mu0.setZero();
  b0 = 2;
  df = num_latent;

  // side information
  F = Fmat;
  use_FtF = comp_FtF;
  if (use_FtF) {
    At_mul_A(FtF, F);
  }

  Uhat.resize(num_latent, F.rows());
  Uhat.setZero();

  beta.resize(num_latent, F.cols());
  beta.setZero();
}

template<class FType>
void MacauPrior<FType>::sample_latents(Eigen::MatrixXd &U, const Eigen::SparseMatrix<double> &mat, double mean_value,
                    const Eigen::MatrixXd &samples, double alpha, const int num_latent) {
  const int N = U.cols();
#pragma omp parallel for
  for(int n = 0; n < N; n++) {
    // TODO: try moving mu + Uhat.col(n) inside sample_latent for speed
    sample_latent_blas(U, n, mat, mean_value, samples, alpha, mu + Uhat.col(n), Lambda, num_latent);
  }
}

template<class FType>
void MacauPrior<FType>::update_prior(const Eigen::MatrixXd &U) {
  // residual:
  Uhat.noalias() = U - Uhat;
  tie(mu, Lambda) = CondNormalWishart(Uhat, mu0, b0, WI + lambda_beta * (beta * beta.transpose()), df + beta.cols());
  // update beta and Uhat:
  sample_beta(U);
}

/** Update beta and Uhat */
template<class FType>
void MacauPrior<FType>::sample_beta(const Eigen::MatrixXd &U) {
  const int num_feat = beta.cols();
  // Ft_y = (U .- mu + Normal(0, Lambda^-1)) * F + sqrt(lambda_beta) * Normal(0, Lambda^-1)
  // Ft_y is [ D x F ] matrix
  MatrixXd Ft_y = A_mul_B( (U + MvNormal_prec_omp(Lambda, U.cols())).colwise() - mu, F) + sqrt(lambda_beta) * MvNormal_prec_omp(Lambda, num_feat);

  if (use_FtF) {
    MatrixXd K(FtF.rows(), FtF.cols());
    K.triangularView<Eigen::Lower>() = FtF;
    K.diagonal() += lambda_beta;
    chol_decomp(K);
    chol_solve_t(K, Ft_y);
    beta = Ft_y;
    compute_uhat(Uhat, F, beta);
  } else {
    // TODO
  }
}

/** global function */
void sample_latent(MatrixXd &s, int mm, const SparseMatrix<double> &mat, double mean_rating,
    const MatrixXd &samples, double alpha, const VectorXd &mu_u, const MatrixXd &Lambda_u,
    const int num_latent)
{
  // TODO: add cholesky update version
  MatrixXd MM = MatrixXd::Zero(num_latent, num_latent);
  VectorXd rr = VectorXd::Zero(num_latent);
  for (SparseMatrix<double>::InnerIterator it(mat, mm); it; ++it) {
    auto col = samples.col(it.row());
    MM.noalias() += col * col.transpose();
    rr.noalias() += col * ((it.value() - mean_rating) * alpha);
  }

  Eigen::LLT<MatrixXd> chol = (Lambda_u + alpha * MM).llt();
  if(chol.info() != Eigen::Success) {
    throw std::runtime_error("Cholesky Decomposition failed!");
  }

  rr.noalias() += Lambda_u * mu_u;
  chol.matrixL().solveInPlace(rr);
  for (int i = 0; i < num_latent; i++) {
    rr[i] += randn0();
  }
  chol.matrixU().solveInPlace(rr);
  s.col(mm).noalias() = rr;
}

void sample_latent_blas(MatrixXd &s, int mm, const SparseMatrix<double> &mat, double mean_rating,
    const MatrixXd &samples, double alpha, const VectorXd &mu_u, const MatrixXd &Lambda_u,
    const int num_latent)
{
  MatrixXd MM = Lambda_u;
  VectorXd rr = VectorXd::Zero(num_latent);
  for (SparseMatrix<double>::InnerIterator it(mat, mm); it; ++it) {
    auto col = samples.col(it.row());
    MM.triangularView<Eigen::Lower>() += alpha * col * col.transpose();
    rr.noalias() += col * ((it.value() - mean_rating) * alpha);
  }

  Eigen::LLT<MatrixXd> chol = MM.llt();
  if(chol.info() != Eigen::Success) {
    throw std::runtime_error("Cholesky Decomposition failed!");
  }

  rr.noalias() += Lambda_u * mu_u;
  chol.matrixL().solveInPlace(rr);
  for (int i = 0; i < num_latent; i++) {
    rr[i] += randn0();
  }
  chol.matrixU().solveInPlace(rr);
  s.col(mm).noalias() = rr;
}

void At_mul_A(Eigen::MatrixXd & result, const Eigen::MatrixXd & F) {
  // TODO: use blas
  result.triangularView<Eigen::Lower>() = F.transpose() * F;
}

Eigen::MatrixXd A_mul_B(const Eigen::MatrixXd & A, const Eigen::MatrixXd & B) {
  // TODO: use blas
  return A * B;
}

Eigen::MatrixXd A_mul_B(const Eigen::MatrixXd & A, const SparseFeat & B) {
  // TODO: use blas
  //return A * B;
  return MatrixXd(1,1);
}

