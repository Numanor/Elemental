/*
   Copyright (c) 2009-2015, Jack Poulson
   All rights reserved.

   This file is part of Elemental and is under the BSD 2-Clause License, 
   which can be found in the LICENSE file in the root directory, or at 
   http://opensource.org/licenses/BSD-2-Clause
*/
#include "El.hpp"
#include "./util.hpp"

namespace El {
namespace lp {
namespace direct {

// The following solves a pair of linear programs in "direct" conic form:
//
//   min c^T x
//   s.t. A x = b, x >= 0,
//
//   max -b^T y
//   s.t. A^T y - z + c = 0, z >= 0,
//
// as opposed to the more general "affine" conic form:
//
//   min c^T x
//   s.t. A x = b, G x + s = h, s >= 0,
//
//   max -b^T y - h^T z
//   s.t. A^T y + G^T z + c = 0, z >= 0
//
// using a simple Infeasible Path Following (IPF) scheme. This routine
// should only be used for academic purposes, as the Mehrotra alternative
// typically requires an order of magnitude fewer iterations.

template<typename Real>
void IPF
( const Matrix<Real>& APre, 
  const Matrix<Real>& bPre, 
  const Matrix<Real>& cPre,
        Matrix<Real>& x,
        Matrix<Real>& y, 
        Matrix<Real>& z,
  const IPFCtrl<Real>& ctrl )
{
    DEBUG_ONLY(CSE cse("lp::direct::IPF"))    

    // Equilibrate the LP by diagonally scaling A
    auto A = APre;
    auto b = bPre;
    auto c = cPre;
    const Int m = A.Height();
    const Int n = A.Width();
    Matrix<Real> dRow, dCol;
    if( ctrl.equilibrate )
    {
        GeomEquil( A, dRow, dCol, ctrl.print );

        DiagonalSolve( LEFT, NORMAL, dRow, b );
        DiagonalSolve( LEFT, NORMAL, dCol, c );
        if( ctrl.primalInit )
            DiagonalScale( LEFT, NORMAL, dCol, x );
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRow, y );
            DiagonalSolve( LEFT, NORMAL, dCol, z );
        }
    }
    else
    {
        Ones( dRow, m, 1 );
        Ones( dCol, n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );

    // TODO: Expose this as a parameter to IPFCtrl
    const bool standardShift = true;
    Initialize
    ( A, b, c, x, y, z, ctrl.primalInit, ctrl.dualInit, standardShift );

    Real relError = 1;
    Matrix<Real> J, d, 
                 rc, rb, rmu, 
                 dx, dy, dz;
#ifndef EL_RELEASE
    Matrix<Real> dxError, dyError, dzError, prod;
#endif
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that x and z are in the cone
        // ===================================
        const Int xNumNonPos = NumNonPositive( x );
        const Int zNumNonPos = NumNonPositive( z );
        if( xNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (xNumNonPos," entries of x were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure
        // ===========================
        const Real mu = Dot(x,z) / n;

        // Check for convergence
        // =====================
        // |primal - dual| / (1 + |primal|) <= tol ?
        // -----------------------------------------
        const Real primObj = Dot(c,x);
        const Real dualObj = -Dot(b,y); 
        const Real objConv = Abs(primObj-dualObj) / (1+Abs(primObj));
        // || r_b ||_2 / (1 + || b ||_2) <= tol ?
        // --------------------------------------
        rb = b;
        rb *= -1;
        Gemv( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);
        // || r_c ||_2 / (1 + || c ||_2) <= tol ?
        // --------------------------------------
        rc = c;
        Gemv( TRANSPOSE, Real(1), A, y, Real(1), rc );
        rc -= z;
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);
        // Now check the pieces
        // --------------------
        relError = Max(Max(objConv,rbConv),rcConv);
        if( ctrl.print )
            Output
            ("iter ",numIts,":\n",Indent(),
             "  |primal - dual| / (1 + |primal|) = ",objConv,"\n",Indent(),
             "  || r_b ||_2 / (1 + || b ||_2)    = ",rbConv,"\n",Indent(),
             "  || r_c ||_2 / (1 + || c ||_2)    = ",rcConv);
        if( relError <= ctrl.targetTol )
            break;
        if( numIts == ctrl.maxIts && relError > ctrl.minTol )
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving minTol=",ctrl.minTol);

        // Compute the search direction
        // ============================

        // r_mu := x o z - tau e
        // ---------------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, x, rmu );
        Shift( rmu, -ctrl.centering*mu );

        if( ctrl.system == FULL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            KKT( A, x, z, J );
            KKTRHS( rc, rb, rmu, z, d );

            // Solve for the direction
            // -----------------------
            try { symm_solve::Overwrite( LOWER, NORMAL, J, d ); }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance of ",ctrl.minTol);
            }
            ExpandSolution( m, n, d, dx, dy, dz );
        }
        else if( ctrl.system == AUGMENTED_KKT )
        {
            // Construct the KKT system
            // ------------------------
            AugmentedKKT( A, x, z, J );
            AugmentedKKTRHS( x, rc, rb, rmu, d );

            // Solve for the direction
            // -----------------------
            try { symm_solve::Overwrite( LOWER, NORMAL, J, d ); }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance of ",ctrl.minTol);
            }
            ExpandAugmentedSolution( x, z, rmu, d, dx, dy, dz );
        }
        else if( ctrl.system == NORMAL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            NormalKKT( A, x, z, J );
            NormalKKTRHS( A, x, z, rc, rb, rmu, dy );

            // Solve for the direction
            // -----------------------
            try { symm_solve::Overwrite( LOWER, NORMAL, J, dy ); }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance of ",ctrl.minTol);
            }
            ExpandNormalSolution( A, c, x, z, rc, rmu, dx, dy, dz );
        }
        else
            LogicError("Invalid KKT system choice");

#ifndef EL_RELEASE
        // Sanity checks
        // -------------
        dxError = rb;
        Gemv( NORMAL, Real(1), A, dx, Real(1), dxError );
        const Real dxErrorNrm2 = Nrm2( dxError );

        dyError = rc;
        Gemv( TRANSPOSE, Real(1), A, dy, Real(1), dyError );
        dyError -= dz;
        const Real dyErrorNrm2 = Nrm2( dyError );

        const Real rmuNrm2 = Nrm2( rmu );
        dzError = rmu;
        prod = dz;
        DiagonalScale( LEFT, NORMAL, x, prod );
        dzError += prod;
        prod = dx;
        DiagonalScale( LEFT, NORMAL, z, prod ); 
        dzError += prod;
        const Real dzErrorNrm2 = Nrm2( dzError );

        if( ctrl.print )
            Output
            ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
             dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
             "|| dyError ||_2 / (1 + || r_c ||_2) = ", 
             dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
             "|| dzError ||_2 / (1 + || r_h ||_2) = ", 
             dzErrorNrm2/(1+rmuNrm2));
#endif

        // Take a step in the computed direction
        // =====================================
        const Real alphaPrimal = MaxStepInPositiveCone( x, dx, Real(1) );
        const Real alphaDual = MaxStepInPositiveCone( z, dz, Real(1) );
        const Real alphaMax = Min(alphaPrimal,alphaDual);
        if( ctrl.print )
            Output("alphaMax = ",alphaMax);
        const Real alpha =
          IPFLineSearch
          ( A, b, c, x, y, z, dx, dy, dz, 
            Real(0.99)*alphaMax,
            ctrl.targetTol*(1+bNrm2), 
            ctrl.targetTol*(1+cNrm2), ctrl.lineSearchCtrl );
        if( ctrl.print )
            Output("alpha = ",alpha);
        Axpy( alpha, dx, x );
        Axpy( alpha, dy, y );
        Axpy( alpha, dz, z );
        if( alpha == Real(0) )
        {
            if( relError <= ctrl.minTol )
                break;
            else
                RuntimeError
                ("Could not achieve minimum tolerance of ",ctrl.minTol);
        }
    }
    SetIndent( indent );

    if( ctrl.equilibrate )
    {
        // Unequilibrate the LP
        DiagonalSolve( LEFT, NORMAL, dCol, x );
        DiagonalSolve( LEFT, NORMAL, dRow, y );
        DiagonalScale( LEFT, NORMAL, dCol, z );
    }
}

template<typename Real>
void IPF
( const AbstractDistMatrix<Real>& APre, 
  const AbstractDistMatrix<Real>& bPre,
  const AbstractDistMatrix<Real>& cPre,
        AbstractDistMatrix<Real>& xPre,
        AbstractDistMatrix<Real>& yPre, 
        AbstractDistMatrix<Real>& zPre, 
  const IPFCtrl<Real>& ctrl )
{
    DEBUG_ONLY(CSE cse("lp::direct::IPF"))    
    const Grid& grid = APre.Grid();
    const int commRank = grid.Rank();

    // Ensure that the inputs have the appropriate read/write properties
    DistMatrix<Real> A(grid), b(grid), c(grid);
    A.Align(0,0);
    b.Align(0,0);
    c.Align(0,0);
    A = APre;
    b = bPre;
    c = cPre;
    ProxyCtrl control;
    control.colConstrain = true;
    control.rowConstrain = true;
    control.colAlign = 0;
    control.rowAlign = 0;
    // NOTE: x does not need to be a read proxy when !ctrl.primalInit
    auto xPtr = ReadWriteProxy<Real,MC,MR>(&xPre,control); auto& x = *xPtr;
    // NOTE: {y,z} do not need to be read proxies when !ctrl.dualInit
    auto yPtr = ReadWriteProxy<Real,MC,MR>(&yPre,control); auto& y = *yPtr;
    auto zPtr = ReadWriteProxy<Real,MC,MR>(&zPre,control); auto& z = *zPtr;

    // Equilibrate the LP by diagonally scaling A
    const Int m = A.Height();
    const Int n = A.Width();
    DistMatrix<Real,MC,STAR> dRow(grid);
    DistMatrix<Real,MR,STAR> dCol(grid);
    if( ctrl.equilibrate )
    {
        GeomEquil( A, dRow, dCol, ctrl.print );

        DiagonalSolve( LEFT, NORMAL, dRow, b );
        DiagonalSolve( LEFT, NORMAL, dCol, c );
        if( ctrl.primalInit )
            DiagonalScale( LEFT, NORMAL, dCol, x );
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRow, y );
            DiagonalSolve( LEFT, NORMAL, dCol, z );
        }
    }
    else
    {
        Ones( dRow, m, 1 );
        Ones( dCol, n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );

    // TODO: Expose this as a parameter to IPFCtrl
    const bool standardShift = true;
    Initialize
    ( A, b, c, x, y, z, ctrl.primalInit, ctrl.dualInit, standardShift );

    Real relError = 1;
    DistMatrix<Real> J(grid), d(grid), 
                     rc(grid), rb(grid), rmu(grid),
                     dx(grid), dy(grid), dz(grid);
    dx.AlignWith( x );
    dz.AlignWith( x );
    rmu.AlignWith( x );
#ifndef EL_RELEASE
    DistMatrix<Real> dxError(grid), dyError(grid), dzError(grid), prod(grid);
    dzError.AlignWith( dz );
#endif
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that x and z are in the cone
        // ===================================
        const Int xNumNonPos = NumNonPositive( x );
        const Int zNumNonPos = NumNonPositive( z );
        if( xNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (xNumNonPos," entries of x were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure
        // ===========================
        const Real mu = Dot(x,z) / n;

        // Check for convergence
        // =====================
        // |primal - dual| / (1 + |primal|) <= tol ?
        // -----------------------------------------
        const Real primObj = Dot(c,x);
        const Real dualObj = -Dot(b,y);
        const Real objConv = Abs(primObj-dualObj) / (1+Abs(primObj));
        // || r_b ||_2 / (1 + || b ||_2) <= tol ?
        // --------------------------------------
        rb = b;
        rb *= -1;
        Gemv( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);
        // || r_c ||_2 / (1 + || c ||_2) <= tol ?
        // --------------------------------------
        rc = c;
        Gemv( TRANSPOSE, Real(1), A, y, Real(1), rc );
        rc -= z;
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);
        // Now check the pieces
        // --------------------
        relError = Max(Max(objConv,rbConv),rcConv);
        if( ctrl.print && commRank == 0 )
            Output
            ("iter ",numIts,":\n",Indent(),
             "  |primal - dual| / (1 + |primal|) = ",objConv,"\n",Indent(),
             "  || r_b ||_2 / (1 + || b ||_2)    = ",rbConv,"\n",Indent(),
             "  || r_c ||_2 / (1 + || c ||_2)    = ",rcConv);
        if( relError <= ctrl.targetTol )
            break;
        if( numIts == ctrl.maxIts && relError > ctrl.minTol )
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving minTol=",ctrl.minTol);

        // Compute the search direction
        // ============================

        // r_mu := x o z - tau e
        // ---------------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, x, rmu );
        Shift( rmu, -ctrl.centering*mu );

        if( ctrl.system == FULL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            KKT( A, x, z, J );
            KKTRHS( rc, rb, rmu, z, d );

            // Solve for the direction
            // -----------------------
            try { symm_solve::Overwrite( LOWER, NORMAL, J, d ); }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandSolution( m, n, d, dx, dy, dz );
        }
        else if( ctrl.system == AUGMENTED_KKT )
        {
            // Construct the KKT system
            // ------------------------
            AugmentedKKT( A, x, z, J );
            AugmentedKKTRHS( x, rc, rb, rmu, d );

            // Solve for the direction
            // -----------------------
            try { symm_solve::Overwrite( LOWER, NORMAL, J, d ); }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandAugmentedSolution( x, z, rmu, d, dx, dy, dz );
        }
        else if( ctrl.system == NORMAL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            NormalKKT( A, x, z, J );
            NormalKKTRHS( A, x, z, rc, rb, rmu, dy );

            // Solve for the direction
            // -----------------------
            try { symm_solve::Overwrite( LOWER, NORMAL, J, dy ); }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandNormalSolution( A, c, x, z, rc, rmu, dx, dy, dz );
        }
        else
            LogicError("Invalid KKT system choice");

#ifndef EL_RELEASE
        // Sanity checks
        // -------------
        dxError = rb;
        Gemv( NORMAL, Real(1), A, dx, Real(1), dxError );
        const Real dxErrorNrm2 = Nrm2( dxError );

        dyError = rc;
        Gemv( TRANSPOSE, Real(1), A, dy, Real(1), dyError );
        dyError -= dz;
        const Real dyErrorNrm2 = Nrm2( dyError );

        const Real rmuNrm2 = Nrm2( rmu );
        dzError = rmu;
        prod = dz;
        DiagonalScale( LEFT, NORMAL, x, prod );
        dzError += prod;
        prod = dx;
        DiagonalScale( LEFT, NORMAL, z, prod );
        dzError += prod;
        const Real dzErrorNrm2 = Nrm2( dzError );

        if( ctrl.print && commRank == 0 )
            Output
            ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
             dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
             "|| dyError ||_2 / (1 + || r_c ||_2) = ",        
             dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
             "|| dzError ||_2 / (1 + || r_h ||_2) = ",        
             dzErrorNrm2/(1+rmuNrm2));
#endif

        // Take a step in the computed direction
        // =====================================
        const Real alphaPrimal = MaxStepInPositiveCone( x, dx, Real(1) );
        const Real alphaDual = MaxStepInPositiveCone( z, dz, Real(1) );
        const Real alphaMax = Min(alphaPrimal,alphaDual);
        if( ctrl.print && commRank == 0 )
            Output("alphaMax = ",alphaMax);
        const Real alpha =
          IPFLineSearch
          ( A, b, c, x, y, z, dx, dy, dz, 
            Real(0.99)*alphaMax,
            ctrl.targetTol*(1+bNrm2), 
            ctrl.targetTol*(1+cNrm2), ctrl.lineSearchCtrl );
        if( ctrl.print && commRank == 0 )
            Output("alpha = ",alpha);
        Axpy( alpha, dx, x );
        Axpy( alpha, dy, y );
        Axpy( alpha, dz, z );
        if( alpha == Real(0) ) 
        {
            if( relError <= ctrl.minTol )
                break;
            else
                RuntimeError
                ("Could not achieve minimum tolerance ",ctrl.minTol);
        }
    }
    SetIndent( indent );

    if( ctrl.equilibrate )
    {
        // Unequilibrate the LP
        DiagonalSolve( LEFT, NORMAL, dCol, x );
        DiagonalSolve( LEFT, NORMAL, dRow, y );
        DiagonalScale( LEFT, NORMAL, dCol, z );
    }
}

template<typename Real>
void IPF
( const SparseMatrix<Real>& APre, 
  const Matrix<Real>& bPre,       const Matrix<Real>& cPre,
        Matrix<Real>& x,                Matrix<Real>& y, 
        Matrix<Real>& z,
  const IPFCtrl<Real>& ctrl )
{
    DEBUG_ONLY(CSE cse("lp::direct::IPF"))    

    // Equilibrate the LP by diagonally scaling A
    auto A = APre;
    auto b = bPre;
    auto c = cPre;
    const Int m = A.Height();
    const Int n = A.Width();
    Matrix<Real> dRow, dCol;
    if( ctrl.equilibrate )
    {
        GeomEquil( A, dRow, dCol, ctrl.print );

        DiagonalSolve( LEFT, NORMAL, dRow, b );
        DiagonalSolve( LEFT, NORMAL, dCol, c );
        if( ctrl.primalInit )
            DiagonalScale( LEFT, NORMAL, dCol, x );
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRow, y );
            DiagonalSolve( LEFT, NORMAL, dCol, z );
        }
    }
    else
    {
        Ones( dRow, m, 1 );
        Ones( dCol, n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );

    vector<Int> map, invMap;
    ldl::NodeInfo info;
    ldl::Separator rootSep;
    // The initialization involves an augmented KKT system, and so we can
    // only reuse the factorization metadata if the this IPM is using the
    // augmented formulation
    // TODO: Expose this as a parameter to IPFCtrl
    const bool standardShift = true;
    if( ctrl.system == AUGMENTED_KKT )
    {
        Initialize
        ( A, b, c, x, y, z, map, invMap, rootSep, info,
          ctrl.primalInit, ctrl.dualInit, standardShift, ctrl.qsdCtrl );
    }
    else
    {
        vector<Int> augMap, augInvMap;
        ldl::NodeInfo augInfo;
        ldl::Separator augRootSep;
        Initialize
        ( A, b, c, x, y, z, augMap, augInvMap, augRootSep, augInfo, 
          ctrl.primalInit, ctrl.dualInit, standardShift, ctrl.qsdCtrl );
    }

    SparseMatrix<Real> J, JOrig;
    ldl::Front<Real> JFront;
    Matrix<Real> d,
                 rc, rb, rmu, 
                 dx, dy, dz;

    Matrix<Real> reg;
    if( ctrl.system == FULL_KKT )
    {
        reg.Resize( m+2*n, 1 );
        for( Int i=0; i<m+2*n; ++i )
        {
            if( i < n )
                reg.Set( i, 0, ctrl.qsdCtrl.regPrimal );
            else 
                reg.Set( i, 0, -ctrl.qsdCtrl.regDual );
        }
    }
    else if( ctrl.system == AUGMENTED_KKT )
    {
        reg.Resize( n+m, 1 );
        for( Int i=0; i<n+m; ++i )
        {
            if( i < n )
                reg.Set( i, 0, ctrl.qsdCtrl.regPrimal );
            else
                reg.Set( i, 0, -ctrl.qsdCtrl.regDual );
        }
    }

    Real relError = 1;
    Matrix<Real> dInner;
#ifndef EL_RELEASE
    Matrix<Real> dxError, dyError, dzError, prod;
#endif
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that x and z are in the cone
        // ===================================
        const Int xNumNonPos = NumNonPositive( x );
        const Int zNumNonPos = NumNonPositive( z );
        if( xNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (xNumNonPos," entries of x were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure
        // ===========================
        const Real mu = Dot(x,z) / n;

        // Check for convergence
        // =====================
        // |primal - dual| / (1 + |primal|) <= tol ?
        // -----------------------------------------
        const Real primObj = Dot(c,x);
        const Real dualObj = -Dot(b,y);
        const Real objConv = Abs(primObj-dualObj) / (1+Abs(primObj));
        // || r_b ||_2 / (1 + || b ||_2) <= tol ?
        // --------------------------------------
        rb = b;
        rb *= -1;
        Multiply( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);
        // || r_c ||_2 / (1 + || c ||_2) <= tol ?
        // --------------------------------------
        rc = c;
        Multiply( TRANSPOSE, Real(1), A, y, Real(1), rc );
        rc -= z;
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);
        // Now check the pieces
        // --------------------
        relError = Max(Max(objConv,rbConv),rcConv);
        if( ctrl.print )
            Output
            ("iter ",numIts,":\n",Indent(),
             "  |primal - dual| / (1 + |primal|) = ",objConv,"\n",Indent(),
             "  || r_b ||_2 / (1 + || b ||_2)    = ",rbConv,"\n",Indent(),
             "  || r_c ||_2 / (1 + || c ||_2)    = ",rcConv);
        if( relError <= ctrl.targetTol )
            break;
        if( numIts == ctrl.maxIts && relError > ctrl.minTol )
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving minTol=",ctrl.minTol);

        // Compute the search direction
        // ============================

        // r_mu := x o z - tau e
        // ---------------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, x, rmu );
        Shift( rmu, -ctrl.centering*mu );

        if( ctrl.system == FULL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            KKT( A, x, z, JOrig, false );
            J = JOrig;
            SymmetricGeomEquil( J, dInner, ctrl.print );
            UpdateRealPartOfDiagonal( J, Real(1), reg );
            if( numIts == 0 )
            {
                NestedDissection( J.LockedGraph(), map, rootSep, info );
                InvertMap( map, invMap );
            }
            JFront.Pull( J, map, info );
            KKTRHS( rc, rb, rmu, z, d );

            // Solve for the direction
            // -----------------------
            try 
            {
                LDL( info, JFront, LDL_2D );
                reg_qsd_ldl::SolveAfter
                ( JOrig, reg, dInner, invMap, info, JFront, d, ctrl.qsdCtrl );
            }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Failed to achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandSolution( m, n, d, dx, dy, dz );
        }
        else if( ctrl.system == AUGMENTED_KKT )
        {
            // Construct the KKT system
            // ------------------------
            AugmentedKKT( A, x, z, JOrig, false );
            J = JOrig;
            SymmetricGeomEquil( J, dInner, ctrl.print );
            UpdateRealPartOfDiagonal( J, Real(1), reg );
            if( ctrl.primalInit && ctrl.dualInit && numIts == 0 )
            {
                NestedDissection( J.LockedGraph(), map, rootSep, info );
                InvertMap( map, invMap );
            }
            JFront.Pull( J, map, info );
            AugmentedKKTRHS( x, rc, rb, rmu, d );

            // Solve for the direction
            // -----------------------
            try
            {
                LDL( info, JFront, LDL_2D );
                reg_qsd_ldl::SolveAfter
                ( JOrig, reg, dInner, invMap, info, JFront, d, ctrl.qsdCtrl );
            }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Failed to achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandAugmentedSolution( x, z, rmu, d, dx, dy, dz );
        }
        else if( ctrl.system == NORMAL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            NormalKKT( A, x, z, J, false );
            // TODO: Add equilibration (need to extend ldl::SolveWith...)
            if( numIts == 0 )
            {
                NestedDissection( J.LockedGraph(), map, rootSep, info );
                InvertMap( map, invMap );
            }
            JFront.Pull( J, map, info );
            NormalKKTRHS( A, x, z, rc, rb, rmu, dy );

            // Solve for the direction
            // -----------------------
            try
            {
                LDL( info, JFront, LDL_2D ); 
                ldl::SolveWithIterativeRefinement
                ( J, invMap, info, JFront, dy, 
                  ctrl.qsdCtrl.relTolRefine, ctrl.qsdCtrl.maxRefineIts );
            }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Failed to achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandNormalSolution( A, c, x, z, rc, rmu, dx, dy, dz );
        }
        else
            LogicError("Invalid KKT system choice");

#ifndef EL_RELEASE
        // Sanity checks
        // -------------
        dxError = rb;
        Multiply( NORMAL, Real(1), A, dx, Real(1), dxError );
        const Real dxErrorNrm2 = Nrm2( dxError );

        dyError = rc;
        Multiply( TRANSPOSE, Real(1), A, dy, Real(1), dyError );
        dyError -= dz;
        const Real dyErrorNrm2 = Nrm2( dyError );

        const Real rmuNrm2 = Nrm2( rmu );
        dzError = rmu;
        prod = dz;
        DiagonalScale( LEFT, NORMAL, x, prod );
        dzError += prod;
        prod = dx;
        DiagonalScale( LEFT, NORMAL, z, prod );
        dzError += prod;
        const Real dzErrorNrm2 = Nrm2( dzError );

        // TODO: Also compute and print the residuals with regularization

        if( ctrl.print )
            Output
            ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
             dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
             "|| dyError ||_2 / (1 + || r_c ||_2) = ",        
             dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
             "|| dzError ||_2 / (1 + || r_h ||_2) = ",        
             dzErrorNrm2/(1+rmuNrm2));
#endif

        // Take a step in the computed direction
        // =====================================
        const Real alphaPrimal = MaxStepInPositiveCone( x, dx, Real(1) );
        const Real alphaDual = MaxStepInPositiveCone( z, dz, Real(1) );
        const Real alphaMax = Min(alphaPrimal,alphaDual);
        if( ctrl.print )
            Output("alphaMax = ",alphaMax);
        const Real alpha = 
          IPFLineSearch
          ( A, b, c, x, y, z, dx, dy, dz, 
            Real(0.99)*alphaMax,
            ctrl.targetTol*(1+bNrm2), 
            ctrl.targetTol*(1+cNrm2), ctrl.lineSearchCtrl );
        if( ctrl.print )
            Output("alpha = ",alpha);
        Axpy( alpha, dx, x );
        Axpy( alpha, dy, y );
        Axpy( alpha, dz, z );
        if( alpha == Real(0) ) 
        {
            if( relError <= ctrl.minTol )
                break;
            else
                RuntimeError
                ("Could not achieve minimum tolerance ",ctrl.minTol);
        }
    }
    SetIndent( indent );

    if( ctrl.equilibrate )
    {
        // Unequilibrate the LP
        DiagonalSolve( LEFT, NORMAL, dCol, x );
        DiagonalSolve( LEFT, NORMAL, dRow, y );
        DiagonalScale( LEFT, NORMAL, dCol, z );
    }
}

template<typename Real>
void IPF
( const DistSparseMatrix<Real>& APre, 
  const DistMultiVec<Real>& bPre,
  const DistMultiVec<Real>& cPre,
        DistMultiVec<Real>& x,
        DistMultiVec<Real>& y, 
        DistMultiVec<Real>& z,
  const IPFCtrl<Real>& ctrl )
{
    DEBUG_ONLY(CSE cse("lp::direct::IPF"))    
    mpi::Comm comm = APre.Comm();
    const int commRank = mpi::Rank(comm);

    // Equilibrate the LP by diagonally scaling A
    auto A = APre;
    auto b = bPre;
    auto c = cPre;
    const Int m = A.Height();
    const Int n = A.Width();
    DistMultiVec<Real> dRow(comm), dCol(comm);
    if( ctrl.equilibrate )
    {
        GeomEquil( A, dRow, dCol, ctrl.print );

        DiagonalSolve( LEFT, NORMAL, dRow, b );
        DiagonalSolve( LEFT, NORMAL, dCol, c );
        if( ctrl.primalInit )
            DiagonalScale( LEFT, NORMAL, dCol, x );
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRow, y );
            DiagonalSolve( LEFT, NORMAL, dCol, z );
        }
    }
    else
    {
        Ones( dRow, m, 1 );
        Ones( dCol, n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );

    DistMap map, invMap;
    ldl::DistNodeInfo info;
    ldl::DistSeparator rootSep;
    // The initialization involves an augmented KKT system, and so we can
    // only reuse the factorization metadata if the this IPM is using the
    // augmented formulation
    // TODO: Expose this as a parameter to IPFCtrl
    const bool standardShift = true;
    if( ctrl.system == AUGMENTED_KKT )
    {
        Initialize
        ( A, b, c, x, y, z, map, invMap, rootSep, info,
          ctrl.primalInit, ctrl.dualInit, standardShift, ctrl.qsdCtrl );
    }
    else
    {
        DistMap augMap, augInvMap;
        ldl::DistNodeInfo augInfo;
        ldl::DistSeparator augRootSep;
        Initialize
        ( A, b, c, x, y, z, augMap, augInvMap, augRootSep, augInfo, 
          ctrl.primalInit, ctrl.dualInit, standardShift, ctrl.qsdCtrl );
    }

    DistSparseMultMeta metaOrig, meta;
    DistSparseMatrix<Real> J(comm), JOrig(comm);
    ldl::DistFront<Real> JFront;
    DistMultiVec<Real> d(comm),
                       rc(comm), rb(comm), rmu(comm), 
                       dx(comm), dy(comm), dz(comm);

    DistMultiVec<Real> reg(comm);
    if( ctrl.system == FULL_KKT )
    {
        reg.Resize( m+2*n, 1 );
        for( Int iLoc=0; iLoc<reg.LocalHeight(); ++iLoc )
        {
            const Int i = reg.GlobalRow(iLoc);
            if( i < n )
                reg.SetLocal( iLoc, 0, ctrl.qsdCtrl.regPrimal );
            else
                reg.SetLocal( iLoc, 0, -ctrl.qsdCtrl.regDual );
        }
    }
    else if( ctrl.system == AUGMENTED_KKT )
    {
        reg.Resize( n+m, 1 );
        for( Int iLoc=0; iLoc<reg.LocalHeight(); ++iLoc )
        {
            const Int i = reg.GlobalRow(iLoc);
            if( i < n )
                reg.SetLocal( iLoc, 0, ctrl.qsdCtrl.regPrimal );
            else
                reg.SetLocal( iLoc, 0, -ctrl.qsdCtrl.regDual );
        }
    }

    Real relError = 1;
    DistMultiVec<Real> dInner(comm);
#ifndef EL_RELEASE
    DistMultiVec<Real> dxError(comm), dyError(comm), dzError(comm), prod(comm);
#endif
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that x and z are in the cone
        // ===================================
        const Int xNumNonPos = NumNonPositive( x );
        const Int zNumNonPos = NumNonPositive( z );
        if( xNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (xNumNonPos," entries of x were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure
        // ===========================
        const Real mu = Dot(x,z) / n;

        // Check for convergence
        // =====================
        // |primal - dual| / (1 + |primal|) <= tol ?
        // -----------------------------------------
        const Real primObj = Dot(c,x);
        const Real dualObj = -Dot(b,y);
        const Real objConv = Abs(primObj-dualObj) / (1+Abs(primObj));
        // || r_b ||_2 / (1 + || b ||_2) <= tol ?
        // --------------------------------------
        rb = b;
        rb *= -1;
        Multiply( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);
        // || r_c ||_2 / (1 + || c ||_2) <= tol ?
        // --------------------------------------
        rc = c;
        Multiply( TRANSPOSE, Real(1), A, y, Real(1), rc );
        rc -= z;
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);
        // Now check the pieces
        // --------------------
        relError = Max(Max(objConv,rbConv),rcConv);
        if( ctrl.print && commRank == 0 )
            Output
            ("iter ",numIts,":\n",Indent(),
             "  |primal - dual| / (1 + |primal|) = ",objConv,"\n",Indent(),
             "  || r_b ||_2 / (1 + || b ||_2)    = ",rbConv,"\n",Indent(),
             "  || r_c ||_2 / (1 + || c ||_2)    = ",rcConv);
        if( relError <= ctrl.targetTol )
            break;
        if( numIts == ctrl.maxIts && relError > ctrl.minTol )
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving minTol=",ctrl.minTol);

        // Compute the search direction
        // ============================

        // r_mu := x o z - tau e
        // ---------------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, x, rmu );
        Shift( rmu, -ctrl.centering*mu );

        if( ctrl.system == FULL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            KKT( A, x, z, JOrig, false );
            // Cache the metadata for the finalized JOrig
            if( numIts == 0 )
                metaOrig = JOrig.InitializeMultMeta();
            else
                JOrig.multMeta = metaOrig;
            J = JOrig;
            SymmetricGeomEquil( J, dInner, ctrl.print );
            UpdateRealPartOfDiagonal( J, Real(1), reg );
            // Cache the metadata for the finalized J
            if( numIts == 0 )
            {
                meta = J.InitializeMultMeta();

                NestedDissection( J.LockedDistGraph(), map, rootSep, info );
                InvertMap( map, invMap );
            }
            else
                J.multMeta = meta;
            JFront.Pull( J, map, rootSep, info );
            KKTRHS( rc, rb, rmu, z, d );

            // Solve for the direction
            // -----------------------
            try
            {
                LDL( info, JFront, LDL_2D );
                reg_qsd_ldl::SolveAfter
                ( JOrig, reg, dInner, invMap, info, JFront, d, ctrl.qsdCtrl );
            }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandSolution( m, n, d, dx, dy, dz );
        }
        else if( ctrl.system == AUGMENTED_KKT )
        {
            // Construct the KKT system
            // ------------------------
            AugmentedKKT( A, x, z, JOrig, false );
            // Cache the metadata for the finalized JOrig
            if( numIts == 0 )
                metaOrig = JOrig.InitializeMultMeta();
            else
                JOrig.multMeta = metaOrig;
            J = JOrig;
            SymmetricGeomEquil( J, dInner, ctrl.print );
            UpdateRealPartOfDiagonal( J, Real(1), reg );
            // Cache the metadata for the finalized J
            if( numIts == 0 )
            {
                meta = J.InitializeMultMeta();
                if( ctrl.primalInit && ctrl.dualInit )
                {
                    NestedDissection( J.LockedDistGraph(), map, rootSep, info );
                    InvertMap( map, invMap );
                }
            }
            else
                J.multMeta = meta;
            JFront.Pull( J, map, rootSep, info );
            AugmentedKKTRHS( x, rc, rb, rmu, d );

            // Solve for the direction
            // -----------------------
            try
            {
                LDL( info, JFront, LDL_2D );
                reg_qsd_ldl::SolveAfter
                ( JOrig, reg, dInner, invMap, info, JFront, d, ctrl.qsdCtrl );
            }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandAugmentedSolution( x, z, rmu, d, dx, dy, dz );
        }
        else if( ctrl.system == NORMAL_KKT )
        {
            // Construct the KKT system
            // ------------------------
            NormalKKT( A, x, z, J, false );
            // Cache the metadata for the finalized J
            if( numIts == 0 )
            {
                meta = J.InitializeMultMeta();
                NestedDissection( J.LockedDistGraph(), map, rootSep, info );
                InvertMap( map, invMap );
            }
            else
                J.multMeta = meta;
            JFront.Pull( J, map, rootSep, info );
            NormalKKTRHS( A, x, z, rc, rb, rmu, dy );

            // Solve for the direction
            // -----------------------
            try
            {
                LDL( info, JFront, LDL_INTRAPIV_1D ); 
                ldl::SolveWithIterativeRefinement
                ( J, invMap, info, JFront, dy, 
                  ctrl.qsdCtrl.relTolRefine, ctrl.qsdCtrl.maxRefineIts );
            }
            catch(...)
            {
                if( relError <= ctrl.minTol )
                    break;
                else
                    RuntimeError
                    ("Could not achieve minimum tolerance ",ctrl.minTol);
            }
            ExpandNormalSolution( A, c, x, z, rc, rmu, dx, dy, dz );
        }
        else
            LogicError("Invalid KKT system choice");

#ifndef EL_RELEASE
        // Sanity checks
        // -------------
        dxError = rb;
        Multiply( NORMAL, Real(1), A, dx, Real(1), dxError );
        const Real dxErrorNrm2 = Nrm2( dxError );

        dyError = rc;
        Multiply( TRANSPOSE, Real(1), A, dy, Real(1), dyError );
        dyError -= dz;
        const Real dyErrorNrm2 = Nrm2( dyError );

        const Real rmuNrm2 = Nrm2( rmu );
        dzError = rmu;
        prod = dz;
        DiagonalScale( LEFT, NORMAL, x, prod );
        dzError += prod;
        prod = dx;
        DiagonalScale( LEFT, NORMAL, z, prod );
        dzError += prod;
        const Real dzErrorNrm2 = Nrm2( dzError );

        // TODO: Also compute and print the residuals with regularization

        if( ctrl.print && commRank == 0 )
            Output
            ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
             dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
             "|| dyError ||_2 / (1 + || r_c ||_2) = ",        
             dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
             "|| dzError ||_2 / (1 + || r_h ||_2) = ",        
             dzErrorNrm2/(1+rmuNrm2));
#endif

        // Take a step in the computed direction
        // =====================================
        const Real alphaPrimal = MaxStepInPositiveCone( x, dx, Real(1) );
        const Real alphaDual = MaxStepInPositiveCone( z, dz, Real(1) );
        const Real alphaMax = Min(alphaPrimal,alphaDual);
        if( ctrl.print && commRank == 0 )
            Output("alphaMax = ",alphaMax);
        const Real alpha = 
          IPFLineSearch
          ( A, b, c, x, y, z, dx, dy, dz, 
            Real(0.99)*alphaMax,
            ctrl.targetTol*(1+bNrm2), 
            ctrl.targetTol*(1+cNrm2), ctrl.lineSearchCtrl );
        if( ctrl.print && commRank == 0 )
            Output("alpha = ",alpha);
        Axpy( alpha, dx, x );
        Axpy( alpha, dy, y );
        Axpy( alpha, dz, z );
        if( alpha == Real(0) ) 
        {
            if( relError <= ctrl.minTol )
                break;
            else
                RuntimeError
                ("Could not achieve minimum tolerance ",ctrl.minTol);
        }
    }
    SetIndent( indent );

    if( ctrl.equilibrate )
    {
        // Unequilibrate the LP
        DiagonalSolve( LEFT, NORMAL, dCol, x );
        DiagonalSolve( LEFT, NORMAL, dRow, y );
        DiagonalScale( LEFT, NORMAL, dCol, z );
    }
}

#define PROTO(Real) \
  template void IPF \
  ( const Matrix<Real>& A, \
    const Matrix<Real>& b, const Matrix<Real>& c, \
          Matrix<Real>& x,       Matrix<Real>& y, \
          Matrix<Real>& z, \
    const IPFCtrl<Real>& ctrl ); \
  template void IPF \
  ( const AbstractDistMatrix<Real>& A, \
    const AbstractDistMatrix<Real>& b, const AbstractDistMatrix<Real>& c, \
          AbstractDistMatrix<Real>& x,       AbstractDistMatrix<Real>& y, \
          AbstractDistMatrix<Real>& z, \
    const IPFCtrl<Real>& ctrl ); \
  template void IPF \
  ( const SparseMatrix<Real>& A, \
    const Matrix<Real>& b,       const Matrix<Real>& c, \
          Matrix<Real>& x,             Matrix<Real>& y, \
          Matrix<Real>& z, \
    const IPFCtrl<Real>& ctrl ); \
  template void IPF \
  ( const DistSparseMatrix<Real>& A, \
    const DistMultiVec<Real>& b,     const DistMultiVec<Real>& c, \
          DistMultiVec<Real>& x,           DistMultiVec<Real>& y, \
          DistMultiVec<Real>& z, \
    const IPFCtrl<Real>& ctrl );

#define EL_NO_INT_PROTO
#define EL_NO_COMPLEX_PROTO
#include "El/macros/Instantiate.h"

} // namespace direct
} // namespace lp
} // namespace El
