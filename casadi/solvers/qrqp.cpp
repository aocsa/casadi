/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "qrqp.hpp"
#include "casadi/core/nlpsol.hpp"

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_CONIC_QRQP_EXPORT
  casadi_register_conic_qrqp(Conic::Plugin* plugin) {
    plugin->creator = Qrqp::creator;
    plugin->name = "qrqp";
    plugin->doc = Qrqp::meta_doc.c_str();
    plugin->version = CASADI_VERSION;
    plugin->options = &Qrqp::options_;
    plugin->deserialize = &Qrqp::deserialize;
    return 0;
  }

  extern "C"
  void CASADI_CONIC_QRQP_EXPORT casadi_load_conic_qrqp() {
    Conic::registerPlugin(casadi_register_conic_qrqp);
  }

  Qrqp::Qrqp(const std::string& name, const std::map<std::string, Sparsity> &st)
    : Conic(name, st) {
  }

  Qrqp::~Qrqp() {
    clear_mem();
  }

  const Options Qrqp::options_
  = {{&Conic::options_},
     {{"max_iter",
       {OT_INT,
        "Maximum number of iterations [1000]."}},
      {"constr_viol_tol",
       {OT_DOUBLE,
        "Constraint violation tolerance [1e-8]."}},
      {"dual_inf_tol",
       {OT_DOUBLE,
        "Dual feasibility violation tolerance [1e-8]"}},
      {"print_header",
       {OT_BOOL,
        "Print header [true]."}},
      {"print_iter",
       {OT_BOOL,
        "Print iterations [true]."}},
      {"print_info",
       {OT_BOOL,
        "Print info [true]."}},
      {"min_lam",
       {OT_DOUBLE,
        "Smallest multiplier treated as inactive for the initial active set [0]."}}
     }
  };

  void Qrqp::init(const Dict& opts) {
    // Initialize the base classes
    Conic::init(opts);

    // Default options
    print_iter_ = true;
    print_header_ = true;
    print_info_ = true;

    // Read user options
    for (auto&& op : opts) {
      if (op.first=="max_iter") {
        p_.max_iter = op.second;
      } else if (op.first=="constr_viol_tol") {
        p_.constr_viol_tol = op.second;
      } else if (op.first=="dual_inf_tol") {
        p_.dual_inf_tol = op.second;
      } else if (op.first=="min_lam") {
        p_.min_lam = op.second;
      } else if (op.first=="print_iter") {
        print_iter_ = op.second;
      } else if (op.first=="print_header") {
        print_header_ = op.second;
      } else if (op.first=="print_info") {
        print_info_ = op.second;
      }
    }

    // Transpose of the Jacobian
    AT_ = A_.T();

    // Assemble KKT system sparsity
    kkt_ = Sparsity::kkt(H_, A_, true, true);

    // Symbolic QR factorization
    kkt_.qr_sparse(sp_v_, sp_r_, prinv_, pc_);

    // Setup memory structure
    set_qp_prob();

    // Allocate memory
    casadi_int sz_w, sz_iw;
    casadi_qp_work(&p_, &sz_iw, &sz_w);
    alloc_iw(sz_iw, true);
    alloc_w(sz_w, true);

    if (print_header_) {
      // Print summary
      print("-------------------------------------------\n");
      print("This is casadi::QRQP\n");
      print("Number of variables:                       %9d\n", nx_);
      print("Number of constraints:                     %9d\n", na_);
      print("Number of nonzeros in H:                   %9d\n", H_.nnz());
      print("Number of nonzeros in A:                   %9d\n", A_.nnz());
      print("Number of nonzeros in KKT:                 %9d\n", kkt_.nnz());
      print("Number of nonzeros in QR(V):               %9d\n", sp_v_.nnz());
      print("Number of nonzeros in QR(R):               %9d\n", sp_r_.nnz());
    }
  }

  void Qrqp::set_qp_prob() {
    p_.sp_a = A_;
    p_.sp_h = H_;
    p_.sp_at = AT_;
    p_.sp_kkt = kkt_;
    p_.sp_v = sp_v_;
    p_.sp_r = sp_r_;
    p_.prinv = get_ptr(prinv_);
    p_.pc = get_ptr(pc_);
    casadi_qp_setup(&p_);
  }

  int Qrqp::init_mem(void* mem) const {
    if (Conic::init_mem(mem)) return 1;
    auto m = static_cast<QrqpMemory*>(mem);
    m->return_status = "";
    return 0;
  }

  int Qrqp::
  eval(const double** arg, double** res, casadi_int* iw, double* w, void* mem) const {
    Conic::eval(arg, res, iw, w, mem);
    auto m = static_cast<QrqpMemory*>(mem);
    // Reset statistics
    for (auto&& s : m->fstats) s.second.reset();
    // Setup data structure
    casadi_qp_data<double> d;
    d.prob = &p_;
    d.nz_h = arg[CONIC_H];
    d.g = arg[CONIC_G];
    d.nz_a = arg[CONIC_A];
    d.verbose = print_info_ ? 1 : 0;
    casadi_qp_init(&d, &iw, &w);
    // Pass bounds on z
    casadi_copy(arg[CONIC_LBX], nx_, d.lbz);
    casadi_copy(arg[CONIC_LBA], na_, d.lbz+nx_);
    casadi_copy(arg[CONIC_UBX], nx_, d.ubz);
    casadi_copy(arg[CONIC_UBA], na_, d.ubz+nx_);
    // Pass initial guess
    casadi_copy(arg[CONIC_X0], nx_, d.z);
    casadi_fill(d.z+nx_, na_, nan);
    casadi_copy(arg[CONIC_LAM_X0], nx_, d.lam);
    casadi_copy(arg[CONIC_LAM_A0], na_, d.lam+nx_);
    // Reset solver
    if (casadi_qp_reset(&d)) return 1;
    while (true) {
      // Prepare QP
      int flag = casadi_qp_prepare(&d);
      // Print iteration progress
      if (print_iter_) {
        if (d.iter % 10 == 0) {
          print("%5s %5s %9s %9s %5s %9s %5s %9s %5s %9s %40s\n",
                "Iter", "Sing", "fk", "|pr|", "con", "|du|", "var",
                "min_R", "con", "last_tau", "Note");
        }
        print("%5d %5d %9.2g %9.2g %5d %9.2g %5d %9.2g %5d %9.2g %40s\n",
              d.iter, d.sing, d.f, d.pr, d.ipr, d.du, d.idu,
              d.mina, d.imina, d.tau, d.msg);
      }
      // Terminate iteration?
      if (flag) break;
      // Make an iteration
      if (casadi_qp_iterate(&d)) break;
      // User interrupt
      InterruptHandler::check();
    }
    // Check return flag
    switch (d.status) {
      case QP_SUCCESS:
        m->return_status = "success";
        break;
      case QP_MAX_ITER:
        m->return_status = "Maximum number of iterations reached";
        m->unified_return_status = SOLVER_RET_LIMITED;
        break;
      case QP_NO_SEARCH_DIR:
        m->return_status = "Failed to calculate search direction";
        break;
    }
    // Get solution
    casadi_copy(&d.f, 1, res[CONIC_COST]);
    casadi_copy(d.z, nx_, res[CONIC_X]);
    casadi_copy(d.lam, nx_, res[CONIC_LAM_X]);
    casadi_copy(d.lam+nx_, na_, res[CONIC_LAM_A]);
    // Return
    if (verbose_) casadi_warning(m->return_status);
    m->success = d.status == QP_SUCCESS;
    return 0;
  }

  void Qrqp::codegen_body(CodeGenerator& g) const {
    g.add_auxiliary(CodeGenerator::AUX_QP);
    if (print_iter_) g.add_auxiliary(CodeGenerator::AUX_PRINTF);
    g.local("d", "struct casadi_qp_data");
    g.local("p", "struct casadi_qp_prob");

    // Setup memory structure
    g << "p.sp_a = " << g.sparsity(A_) << ";\n";
    g << "p.sp_h = " << g.sparsity(H_) << ";\n";
    g << "p.sp_at = " << g.sparsity(AT_) << ";\n";
    g << "p.sp_kkt = " << g.sparsity(kkt_) << ";\n";
    g << "p.sp_v = " << g.sparsity(sp_v_) << ";\n";
    g << "p.sp_r = " << g.sparsity(sp_r_) << ";\n";
    g << "p.prinv = " << g.constant(prinv_) << ";\n";
    g << "p.pc =  " << g.constant(pc_) << ";\n";
    g << "casadi_qp_setup(&p);\n";

    // Copy options
    g << "p.max_iter = " << p_.max_iter << ";\n";
    g << "p.min_lam = " << p_.min_lam << ";\n";
    g << "p.constr_viol_tol = " << p_.constr_viol_tol << ";\n";
    g << "p.dual_inf_tol = " << p_.dual_inf_tol << ";\n";

    // Setup data structure
    g << "d.prob = &p;\n";
    g << "d.nz_h = arg[" << CONIC_H << "];\n";
    g << "d.g = arg[" << CONIC_G << "];\n";
    g << "d.nz_a = arg[" << CONIC_A << "];\n";
    g << "d.verbose = " << (print_info_ ? 1 : 0) << ";\n";
    g << "casadi_qp_init(&d, &iw, &w);\n";

    g.comment("Pass bounds on z");
    g.copy_default("arg[" + str(CONIC_LBX)+ "]", nx_, "d.lbz", "-casadi_inf", false);
    g.copy_default("arg[" + str(CONIC_LBA)+ "]", na_, "d.lbz+" + str(nx_), "-casadi_inf", false);
    g.copy_default("arg[" + str(CONIC_UBX)+ "]", nx_, "d.ubz", "casadi_inf", false);
    g.copy_default("arg[" + str(CONIC_UBA)+ "]", na_, "d.ubz+" + str(nx_), "casadi_inf", false);

    g.comment("Pass initial guess");
    g.copy_default("arg[" + str(CONIC_X0)+ "]", nx_, "d.z", "0", false);
    g << g.fill("d.z+"+str(nx_), na_, "NAN") << "\n";
    g.copy_default("arg[" + str(CONIC_LAM_X0)+ "]", nx_, "d.lam", "0", false);
    g.copy_default("arg[" + str(CONIC_LAM_A0)+ "]", na_, "d.lam+" + str(nx_), "0", false);

    g.comment("Solve QP");
    g << "if (casadi_qp_reset(&d)) return 1;\n";
    g << "while (1) {\n";
    g << "if (casadi_qp_prepare(&d)) break;\n";
    if (print_iter_) {
      g << "if (d.iter % 10 == 0) {\n";
      g << g.printf("%5s %5s %9s %9s %5s %9s %5s %9s %5s %9s %40s\\n",
              {"\"Iter\"", "\"Sing\"", "\"fk\"", "\"|pr|\"", "\"con\"", "\"|du|\"", "\"var\"",
              "\"min_R\"", "\"con\"", "\"last_tau\"", "\"Note\""}) << "\n";
      g << "}\n";
      g << g.printf("%5d %5d %9.2g %9.2g %5d %9.2g %5d %9.2g %5d %9.2g %40s\\n",
              {"(int)d.iter", "(int)d.sing", "d.f", "d.pr", "(int)d.ipr", "d.du", "(int)d.idu",
              "d.mina", "(int)d.imina", "d.tau", "d.msg"}) << "\n";
    }
    g << "if (casadi_qp_iterate(&d)) break;\n";
    g << "}\n";

    g.comment("Get solution");
    g.copy_check("&d.f", 1, "res[" + str(CONIC_COST) + "]", false, true);
    g.copy_check("d.z", nx_, "res[" + str(CONIC_X) + "]", false, true);
    g.copy_check("d.lam", nx_, "res[" + str(CONIC_LAM_X) + "]", false, true);
    g.copy_check("d.lam+"+str(nx_), na_, "res[" + str(CONIC_LAM_A) + "]", false, true);

    g << "return d.status != QP_SUCCESS;\n";
  }

  Dict Qrqp::get_stats(void* mem) const {
    Dict stats = Conic::get_stats(mem);
    auto m = static_cast<QrqpMemory*>(mem);
    stats["return_status"] = m->return_status;
    return stats;
  }

  Qrqp::Qrqp(DeserializingStream& s) : Conic(s) {
    s.version("Qrqp", 1);
    s.unpack("Qrqp::AT", AT_);
    s.unpack("Qrqp::kkt", kkt_);
    s.unpack("Qrqp::sp_v", sp_v_);
    s.unpack("Qrqp::sp_r", sp_r_);
    s.unpack("Qrqp::prinv", prinv_);
    s.unpack("Qrqp::pc", pc_);
    s.unpack("Qrqp::print_iter", print_iter_);
    s.unpack("Qrqp::print_header", print_header_);
    s.unpack("Qrqp::print_info", print_info_);
    set_qp_prob();
    s.unpack("Qrqp::max_iter", p_.max_iter);
    s.unpack("Qrqp::min_lam", p_.min_lam);
    s.unpack("Qrqp::constr_viol_tol", p_.constr_viol_tol);
    s.unpack("Qrqp::dual_inf_tol", p_.dual_inf_tol);
  }

  void Qrqp::serialize_body(SerializingStream &s) const {
    Conic::serialize_body(s);

    s.version("Qrqp", 1);
    s.pack("Qrqp::AT", AT_);
    s.pack("Qrqp::kkt", kkt_);
    s.pack("Qrqp::sp_v", sp_v_);
    s.pack("Qrqp::sp_r", sp_r_);
    s.pack("Qrqp::prinv", prinv_);
    s.pack("Qrqp::pc", pc_);
    s.pack("Qrqp::print_iter", print_iter_);
    s.pack("Qrqp::print_header", print_header_);
    s.pack("Qrqp::print_info", print_info_);
    s.pack("Qrqp::max_iter", p_.max_iter);
    s.pack("Qrqp::min_lam", p_.min_lam);
    s.pack("Qrqp::constr_viol_tol", p_.constr_viol_tol);
    s.pack("Qrqp::dual_inf_tol", p_.dual_inf_tol);
  }

} // namespace casadi
