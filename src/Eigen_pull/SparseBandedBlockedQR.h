// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2017 Jan Svoboda <jan.svoboda@usi.ch>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_SPARSE_BANDED_BLOCKED_QR_H
#define EIGEN_SPARSE_BANDED_BLOCKED_QR_H

#include <ctime>
#include <typeinfo>
#include <shared_mutex>
#include "SparseBlockCOO.h"
#include "eigen_extras.h"

namespace Eigen {

	template<typename MatrixType, typename OrderingType, int SuggestedBlockCols, bool MultiThreading> class SparseBandedBlockedQR;
	template<typename SparseBandedBlockedQRType> struct SparseBandedBlockedQRMatrixQReturnType;
	template<typename SparseBandedBlockedQRType> struct SparseBandedBlockedQRMatrixQTransposeReturnType;
	template<typename SparseBandedBlockedQRType, typename Derived> struct SparseBandedBlockedQR_QProduct;
	namespace internal {

		// traits<SparseBandedBlockedQRMatrixQ[Transpose]>
		template <typename SparseBandedBlockedQRType> struct traits<SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType> >
		{
			typedef typename SparseBandedBlockedQRType::MatrixType ReturnType;
			typedef typename ReturnType::StorageIndex StorageIndex;
			typedef typename ReturnType::StorageKind StorageKind;
			enum {
				RowsAtCompileTime = Dynamic,
				ColsAtCompileTime = Dynamic
			};
		};

		template <typename SparseBandedBlockedQRType> struct traits<SparseBandedBlockedQRMatrixQTransposeReturnType<SparseBandedBlockedQRType> >
		{
			typedef typename SparseBandedBlockedQRType::MatrixType ReturnType;
		};

		template <typename SparseBandedBlockedQRType, typename Derived> struct traits<SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived> >
		{
			typedef typename Derived::PlainObject ReturnType;
		};

		// SparseBandedBlockedQR_traits
		template <typename T> struct SparseBandedBlockedQR_traits {  };
		template <class T, int Rows, int Cols, int Options> struct SparseBandedBlockedQR_traits<Matrix<T, Rows, Cols, Options>> {
			typedef Matrix<T, Rows, 1, Options> Vector;
		};
		template <class Scalar, int Options, typename Index> struct SparseBandedBlockedQR_traits<SparseMatrix<Scalar, Options, Index>> {
			typedef SparseVector<Scalar, Options> Vector;
		};
	} // End namespace internal

	/**
	  * \ingroup SparseBandedBlockedQR_Module
	  * \class SparseBandedBlockedQR
	  * \brief Sparse Householder QR Factorization for banded matrices
	  * This implementation is not rank revealing and uses Eigen::HouseholderQR for solving the dense blocks.
	  *
	  * Q is the orthogonal matrix represented as products of Householder reflectors.
	  * Use matrixQ() to get an expression and matrixQ().transpose() to get the transpose.
	  * You can then apply it to a vector.
	  *
	  * R is the sparse triangular or trapezoidal matrix. The later occurs when A is rank-deficient.
	  * matrixR().topLeftCorner(rank(), rank()) always returns a triangular factor of full rank.
	  *
	  * \tparam _MatrixType The type of the sparse matrix A, must be a column-major SparseMatrix<>
	  * \tparam _OrderingType The fill-reducing ordering method. See the \link OrderingMethods_Module
	  *  OrderingMethods \endlink module for the list of built-in and external ordering methods.
	  *
	  * \implsparsesolverconcept
	  *
	  * \warning The input sparse matrix A must be in compressed mode (see SparseMatrix::makeCompressed()).
	  *
	  */
	template<typename _MatrixType, typename _OrderingType, int _SuggestedBlockCols = 2, bool _MultiThreading = false>
	class SparseBandedBlockedQR : public SparseSolverBase<SparseBandedBlockedQR<_MatrixType, _OrderingType, _SuggestedBlockCols, _MultiThreading> >
	{
	protected:
		typedef SparseSolverBase<SparseBandedBlockedQR<_MatrixType, _OrderingType, _SuggestedBlockCols, _MultiThreading> > Base;
		using Base::m_isInitialized;
	public:
		using Base::_solve_impl;
		typedef _MatrixType MatrixType;
		typedef _OrderingType OrderingType;
		typedef typename MatrixType::Scalar Scalar;
		typedef typename MatrixType::RealScalar RealScalar;
		typedef typename MatrixType::StorageIndex StorageIndex;
		typedef Matrix<StorageIndex, Dynamic, 1> IndexVector;
		typedef Matrix<Scalar, Dynamic, 1> ScalarVector;

		typedef SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQR> MatrixQType;
		typedef SparseMatrix<Scalar, ColMajor, StorageIndex> MatrixRType;
		typedef PermutationMatrix<Dynamic, Dynamic, StorageIndex> PermutationType;

		template <typename IndexType>
		struct BlockInfo {
			IndexType rowIdx;
			IndexType colIdx;
			IndexType numRows;
			IndexType numCols;

			BlockInfo()
				: rowIdx(0), colIdx(0), numRows(0), numCols(0) {
			}

			BlockInfo(const IndexType &ri, const IndexType &ci, const IndexType &nr, const IndexType &nc)
				: rowIdx(ri), colIdx(ci), numRows(nr), numCols(nc) {
			}
		};

		typedef BlockInfo<StorageIndex> MatrixBlockInfo;
		typedef std::map<StorageIndex, MatrixBlockInfo> BlockInfoMap;
		typedef std::vector<StorageIndex> BlockInfoMapOrder;

		enum {
			ColsAtCompileTime = MatrixType::ColsAtCompileTime,
			MaxColsAtCompileTime = MatrixType::MaxColsAtCompileTime
		};

	public:
		SparseBandedBlockedQR() : m_analysisIsok(false), m_lastError(""), m_useDefaultThreshold(true), m_useMultiThreading(_MultiThreading)
		{ }
	
		/** Construct a QR factorization of the matrix \a mat.
		  *
		  * \warning The matrix \a mat must be in compressed mode (see SparseMatrix::makeCompressed()).
		  *
		  * \sa compute()
		  */
		explicit SparseBandedBlockedQR(const MatrixType& mat) : m_analysisIsok(false), m_lastError(""), m_useDefaultThreshold(true), m_useMultiThreading(_MultiThreading)
		{
			compute(mat);
		}

		/** Computes the QR factorization of the sparse matrix \a mat.
		  *
		  * \warning The matrix \a mat must be in compressed mode (see SparseMatrix::makeCompressed()).
		  *
		  * \sa analyzePattern(), factorize()
		  */
		void compute(const MatrixType& mat)
		{
			analyzePattern(mat);
			factorize(mat);
		}
		void analyzePattern(const MatrixType& mat);
		void factorize(const MatrixType& mat);

		/** \returns the number of rows of the represented matrix.
		  */
		inline Index rows() const { return m_pmat.rows(); }

		/** \returns the number of columns of the represented matrix.
		  */
		inline Index cols() const { return m_pmat.cols(); }

		/** \returns a const reference to the \b sparse upper triangular matrix R of the QR factorization.
		  * \warning The entries of the returned matrix are not sorted. This means that using it in algorithms
		  *          expecting sorted entries will fail. This include random coefficient accesses (SpaseMatrix::coeff()),
		  *          and coefficient-wise operations. Matrix products and triangular solves are fine though.
		  *
		  * To sort the entries, you can assign it to a row-major matrix, and if a column-major matrix
		  * is required, you can copy it again:
		  * \code
		  * SparseMatrix<double>          R  = qr.matrixR();  // column-major, not sorted!
		  * SparseMatrix<double,RowMajor> Rr = qr.matrixR();  // row-major, sorted
		  * SparseMatrix<double>          Rc = Rr;            // column-major, sorted
		  * \endcode
		  */
		const MatrixRType& matrixR() const { return m_R; }

		/** \returns the number of non linearly dependent columns as determined by the pivoting threshold.
		  *
		  * \sa setPivotThreshold()
		  */
		Index rank() const
		{
			eigen_assert(m_isInitialized && "The factorization should be called first, use compute()");
			return m_nonzeropivots;
		}

		/** \returns an expression of the matrix Q as products of sparse Householder reflectors.
		* The common usage of this function is to apply it to a dense matrix or vector
		* \code
		* VectorXd B1, B2;
		* // Initialize B1
		* B2 = matrixQ() * B1;
		* \endcode
		*
		* To get a plain SparseMatrix representation of Q:
		* \code
		* SparseMatrix<double> Q;
		* Q = SparseBandedBlockedQR<SparseMatrix<double> >(A).matrixQ();
		* \endcode
		* Internally, this call simply performs a sparse product between the matrix Q
		* and a sparse identity matrix. However, due to the fact that the sparse
		* reflectors are stored unsorted, two transpositions are needed to sort
		* them before performing the product.
		*/
		SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQR> matrixQ() const
		{
			return SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQR>(*this);
		}

		/** \returns a const reference to the column permutation P that was applied to A such that A*P = Q*R
		* It is the combination of the fill-in reducing permutation and numerical column pivoting.
		*/
		const PermutationType& colsPermutation() const
		{
			eigen_assert(m_isInitialized && "Decomposition is not initialized.");
			return m_outputPerm_c;
		}

		const PermutationType& rowsPermutation() const {
			eigen_assert(m_isInitialized && "Decomposition is not initialized.");
			return this->m_rowPerm;
		}

		/** \returns A string describing the type of error.
		  * This method is provided to ease debugging, not to handle errors.
		  */
		std::string lastErrorMessage() const { return m_lastError; }

		/** \internal */
		template<typename Rhs, typename Dest>
		bool _solve_impl(const MatrixBase<Rhs> &B, MatrixBase<Dest> &dest) const
		{
			eigen_assert(m_isInitialized && "The factorization should be called first, use compute()");
			eigen_assert(this->rows() == B.rows() && "SparseBandedBlockedQR::solve() : invalid number of rows in the right hand side matrix");

			Index rank = this->rank();

			// Compute Q^T * b;
			typename Dest::PlainObject y, b;
			y = this->matrixQ().transpose() * B;
			b = y;

			// Solve with the triangular matrix R
			y.resize((std::max<Index>)(cols(), y.rows()), y.cols());
			y.topRows(rank) = this->matrixR().topLeftCorner(rank, rank).template triangularView<Upper>().solve(b.topRows(rank));
			y.bottomRows(y.rows() - rank).setZero();

			dest = y.topRows(cols());

			m_info = Success;
			return true;
		}

		/** Sets the threshold that is used to determine linearly dependent columns during the factorization.
		  *
		  * In practice, if during the factorization the norm of the column that has to be eliminated is below
		  * this threshold, then the entire column is treated as zero, and it is moved at the end.
		  */
		void setPivotThreshold(const RealScalar& threshold)
		{
			m_useDefaultThreshold = false;
			m_threshold = threshold;
		}

		/** \returns the solution X of \f$ A X = B \f$ using the current decomposition of A.
		  *
		  * \sa compute()
		  */
		template<typename Rhs>
		inline const Solve<SparseBandedBlockedQR, Rhs> solve(const MatrixBase<Rhs>& B) const
		{
			eigen_assert(m_isInitialized && "The factorization should be called first, use compute()");
			eigen_assert(this->rows() == B.rows() && "SparseBandedBlockedQR::solve() : invalid number of rows in the right hand side matrix");
			return Solve<SparseBandedBlockedQR, Rhs>(*this, B.derived());
		}
		template<typename Rhs>
		inline const Solve<SparseBandedBlockedQR, Rhs> solve(const SparseMatrixBase<Rhs>& B) const
		{
			eigen_assert(m_isInitialized && "The factorization should be called first, use compute()");
			eigen_assert(this->rows() == B.rows() && "SparseBandedBlockedQR::solve() : invalid number of rows in the right hand side matrix");
			return Solve<SparseBandedBlockedQR, Rhs>(*this, B.derived());
		}

		/** \brief Reports whether previous computation was successful.
		  *
		  * \returns \c Success if computation was successful,
		  *          \c NumericalIssue if the QR factorization reports a numerical problem
		  *          \c InvalidInput if the input matrix is invalid
		  *
		  * \sa iparm()
		  */
		ComputationInfo info() const
		{
			eigen_assert(m_isInitialized && "Decomposition is not initialized.");
			return m_info;
		}

	protected:
		typedef SparseMatrix<Scalar, ColMajor, StorageIndex> MatrixQStorageType;
		typedef SparseBlockCOO<BlockYTY<StorageIndex>, StorageIndex> SparseBlockYTY;

		bool m_analysisIsok;
		bool m_factorizationIsok;
		mutable ComputationInfo m_info;
		std::string m_lastError;
		MatrixQStorageType m_pmat;            // Temporary matrix
		MatrixRType m_R;                // The triangular factor matrix
		SparseBlockYTY m_blocksYT;
		PermutationType m_outputPerm_c; // The final column permutation (for compatibility here, set to identity)
		PermutationType m_rowPerm;
		RealScalar m_threshold;         // Threshold to determine null Householder reflections
		bool m_useDefaultThreshold;     // Use default threshold
		Index m_nonzeropivots;          // Number of non zero pivots found

		BlockInfoMap m_blockMap;
		BlockInfoMapOrder m_blockOrder;

		bool m_useMultiThreading;

		template <typename, typename > friend struct SparseBandedBlockedQR_QProduct;

	};

	/** \brief Preprocessing step of a QR factorization
	  *
	  * \warning The matrix \a mat must be in compressed mode (see SparseMatrix::makeCompressed()).
	  *
	  * In this step, the fill-reducing permutation is computed and applied to the columns of A
	  * and the column elimination tree is computed as well. Only the sparsity pattern of \a mat is exploited.
	  *
	  * \note In this step it is assumed that there is no empty row in the matrix \a mat.
	  */
	template <typename IndexType>
	struct RowRange {
		IndexType origIdx;
		IndexType start;
		IndexType end;
		IndexType length;

		RowRange() : start(0), end(0), length(0) {
		}

		RowRange(const IndexType &origIdx, const IndexType &start, const IndexType &end)
			: origIdx(origIdx), start(start), end(end) {
			this->length = this->end - this->start + 1;
		}

		bool operator<(const RowRange &rr) const {
			if (this->start < rr.start) {
				return true;
			}
			else {
				return (rr.start < this->end) && (this->end > rr.end);
			}
		}
	};
	
	template <typename MatrixType, typename OrderingType, int SuggestedBlockCols, bool MultiThreading>
	void SparseBandedBlockedQR<MatrixType, OrderingType, SuggestedBlockCols, MultiThreading>::analyzePattern(const MatrixType& mat)
	{
		typedef RowRange<MatrixType::StorageIndex> MatrixRowRange;
		typedef std::map<MatrixType::StorageIndex, MatrixType::StorageIndex> BlockBandSize;
		typedef SparseMatrix<Scalar, RowMajor, MatrixType::StorageIndex> RowMajorMatrixType;

		Index n = mat.cols();
		Index m = mat.rows();
		Index diagSize = (std::min)(m, n);

		// Go through the matrix and reorder rows so that the matrix gets as-banded-as-possible structure
		BlockBandSize bandWidths, bandHeights;
		RowMajorMatrixType rmMat(mat);
		std::vector<MatrixRowRange> rowRanges;
		for (MatrixType::StorageIndex j = 0; j < rmMat.rows(); j++) {
			RowMajorMatrixType::InnerIterator rowIt(rmMat, j);
			MatrixType::StorageIndex startIdx = rowIt.index();
			MatrixType::StorageIndex endIdx = startIdx;
			while (++rowIt) { endIdx = rowIt.index(); }	// FixMe: Is there a better way?
			rowRanges.push_back(MatrixRowRange(j, startIdx, endIdx));

			MatrixType::StorageIndex bw = endIdx - startIdx + 1;
			if (bandWidths.find(startIdx) == bandWidths.end()) {
				bandWidths.insert(std::make_pair(startIdx, bw));
			}
			else {
				if (bandWidths.at(startIdx) < bw) {
					bandWidths.at(startIdx) = bw;
				}
			}

			if (bandHeights.find(startIdx) == bandHeights.end()) {
				bandHeights.insert(std::make_pair(startIdx, 1));
			}
			else {
				bandHeights.at(startIdx) += 1;
			}
		}

		// Sort the rows to form as-banded-as-possible matrix
		std::sort(rowRanges.begin(), rowRanges.end(), [](const MatrixRowRange &lhs, const MatrixRowRange &rhs) {
			return (lhs.start < rhs.start);
		});

		// Search for the blocks		
		MatrixType::StorageIndex maxColStep = 0;
		for(MatrixType::StorageIndex j = 0; j < rowRanges.size() - 1; j++) {
			if ((rowRanges.at(j + 1).start - rowRanges.at(j).start) > maxColStep) {
				maxColStep = (rowRanges.at(j + 1).start - rowRanges.at(j).start);
			}
		}

		// And now create the final blocks
		Eigen::Matrix<MatrixType::StorageIndex, Dynamic, 1> permIndices(rowRanges.size());
		MatrixType::StorageIndex rowIdx = 0;
		for (auto it = rowRanges.begin(); it != rowRanges.end(); ++it, rowIdx++) {
			permIndices(it->origIdx) = rowIdx;

			if (std::find(this->m_blockOrder.begin(), this->m_blockOrder.end(), it->start) == this->m_blockOrder.end()) {
				this->m_blockOrder.push_back(it->start);
				this->m_blockMap.insert(std::make_pair(it->start, MatrixBlockInfo(rowIdx, it->start, bandHeights.at(it->start), bandWidths.at(it->start))));
			}
		}
		this->m_rowPerm = PermutationType(permIndices);
		
		// Merge several blocks together
		BlockInfoMap newBlockMap;
		BlockInfoMapOrder newBlockOrder;
		MatrixBlockInfo firstBlock;
		MatrixType::StorageIndex prevBlockEndCol = 0;
		MatrixType::StorageIndex sumRows = 0;
		MatrixType::StorageIndex numCols = 0;
		MatrixType::StorageIndex colStep = 0;
		MatrixType::StorageIndex blockOverlap = 0;
		auto it = this->m_blockOrder.begin();
		for (; it != this->m_blockOrder.end(); ++it) {
			if (sumRows == 0) {
				firstBlock = this->m_blockMap.at(*it);
			}

			sumRows += this->m_blockMap.at(*it).numRows;
			numCols = (this->m_blockMap.at(*it).colIdx + this->m_blockMap.at(*it).numCols) - firstBlock.colIdx;
			colStep = this->m_blockMap.at(*it).colIdx - firstBlock.colIdx;

			if ((newBlockOrder.empty() || colStep >= maxColStep / 2 - 1) && sumRows > numCols && numCols >= SuggestedBlockCols) {
				newBlockOrder.push_back(firstBlock.colIdx);
				newBlockMap.insert(std::make_pair(firstBlock.colIdx, MatrixBlockInfo(firstBlock.rowIdx, firstBlock.colIdx, sumRows, numCols)));

				sumRows = 0;
				prevBlockEndCol = firstBlock.colIdx + numCols;
			}
		}
		// Process also last collection
		--it;
		if (sumRows > 0) {
			colStep = this->m_blockMap.at(*it).colIdx - firstBlock.colIdx;
		
			if (colStep >= maxColStep / 2 && sumRows > numCols && numCols >= SuggestedBlockCols) {
				newBlockOrder.push_back(firstBlock.colIdx);
				int numCols = (this->m_blockMap.at(*it).colIdx + this->m_blockMap.at(*it).numCols) - firstBlock.colIdx;
				newBlockMap.insert(std::make_pair(firstBlock.colIdx, MatrixBlockInfo(firstBlock.rowIdx, firstBlock.colIdx, sumRows, numCols)));
			}
			else {
				firstBlock = newBlockMap[newBlockOrder.back()];
				int numCols = (this->m_blockMap.at(*it).colIdx + this->m_blockMap.at(*it).numCols) - firstBlock.colIdx;
				newBlockMap[newBlockOrder.back()] = MatrixBlockInfo(firstBlock.rowIdx, firstBlock.colIdx, firstBlock.numRows + sumRows, numCols);
			}
		}
		// Update block structure
		this->m_blockOrder = newBlockOrder;
		this->m_blockMap = newBlockMap;
		
		MatrixType::StorageIndex numBlocks = this->m_blockOrder.size();

		m_R.resize(mat.rows(), mat.cols());

		m_analysisIsok = true;
	}

/** \brief Performs the numerical QR factorization of the input matrix
  *
  * The function SparseBandedBlockedQR::analyzePattern(const MatrixType&) must have been called beforehand with
  * a matrix having the same sparsity pattern than \a mat.
  *
  * \param mat The sparse column-major matrix
  */
template <typename MatrixType, typename OrderingType, int SuggestedBlockCols, bool MultiThreading>
void SparseBandedBlockedQR<MatrixType, OrderingType, SuggestedBlockCols, MultiThreading>::factorize(const MatrixType& mat)
{
	// Not rank-revealing, column permutation is identity
	m_outputPerm_c.setIdentity(mat.cols());

	m_pmat = (this->m_rowPerm * mat);

	typedef Matrix<Scalar, Dynamic, Dynamic> DenseMatrixType;

	Eigen::TripletArray<Scalar, typename MatrixType::Index> Rvals(2 * mat.nonZeros());

	Eigen::HouseholderQR<DenseMatrixType> houseqr;
	Index numBlocks = this->m_blockOrder.size();

	// Prepare the first block
	MatrixBlockInfo bi = this->m_blockMap.at(this->m_blockOrder.at(0));
	DenseMatrixType Ji = m_pmat.block(bi.rowIdx, bi.colIdx, bi.numRows, bi.numCols);
	Index activeRows = bi.numRows;
	Index numZeros = 0;
	// Some auxiliary variables for later
	MatrixBlockInfo biNext;
	Index colIncrement, blockOverlap;
	for (Index i = 0; i < numBlocks; i++) {
		// Current block info
		bi = this->m_blockMap.at(this->m_blockOrder.at(i));

		// Solve the current dense block using Householder QR
		houseqr.compute(Ji);

		// Update matrices T and Y
		MatrixXd T = MatrixXd::Zero(bi.numCols, bi.numCols);
		MatrixXd Y = MatrixXd::Zero(activeRows, bi.numCols);
		VectorXd v = VectorXd::Zero(activeRows);
		VectorXd z = VectorXd::Zero(activeRows);

		v(0) = 1.0;
		v.segment(1, houseqr.householderQ().essentialVector(0).rows()) = houseqr.householderQ().essentialVector(0);
		Y.col(0) = v;
		T(0, 0) = -houseqr.hCoeffs()(0);
		for (MatrixType::StorageIndex bc = 1; bc < bi.numCols; bc++) {
			v.setZero();
			v(bc) = 1.0;
			v.segment(bc + 1, houseqr.householderQ().essentialVector(bc).rows()) = houseqr.householderQ().essentialVector(bc);

			z = -houseqr.hCoeffs()(bc) * (T * (Y.transpose() * v));

			Y.col(bc) = v;
			T.col(bc) = z;
			T(bc, bc) = -houseqr.hCoeffs()(bc);
		}

		/*
		* Save current Y and T. Can be saved separately as upper (diagoal) part of Y & T and lower (off-diagonal) part of Y & Y
		*/
		Index diagIdx = bi.colIdx;
		m_blocksYT.insert(SparseBlockYTY::Element(diagIdx, diagIdx, BlockYTY<StorageIndex>(Y, T, numZeros)));
		
		// Get the R part of the dense QR decomposition 
		MatrixXd V = houseqr.matrixQR().template triangularView<Upper>();

		// Update sparse R with the rows solved in this step
		int solvedRows = (i == numBlocks - 1) ? bi.numRows : this->m_blockMap.at(this->m_blockOrder.at(i + 1)).colIdx - bi.colIdx;
		for (MatrixType::StorageIndex br = 0; br < solvedRows; br++) {
			for (MatrixType::StorageIndex bc = 0; bc < bi.numCols; bc++) {
				Rvals.add_if_nonzero(diagIdx + br, bi.colIdx + bc, V(br, bc));
			}
		}

		// If this is not the last block, proceed to the next block
		if (i < numBlocks - 1) {
			biNext = this->m_blockMap.at(this->m_blockOrder.at(i + 1));
			blockOverlap = (bi.colIdx + bi.numCols) - biNext.colIdx;
			colIncrement = bi.numCols - blockOverlap;
			activeRows = bi.numRows + biNext.numRows - colIncrement;
			numZeros = (biNext.rowIdx + biNext.numRows) - activeRows - biNext.colIdx;
			numZeros = (numZeros < 0) ? 0 : numZeros;

			MatrixType::StorageIndex numCols = (biNext.numCols >= blockOverlap) ? biNext.numCols : blockOverlap;
			Ji = m_pmat.block(bi.rowIdx + colIncrement, biNext.colIdx, activeRows, numCols).toDense();
			if (blockOverlap > 0) {
				Ji.block(0, 0, activeRows - biNext.numRows, blockOverlap) = V.block(colIncrement, colIncrement, activeRows - biNext.numRows, blockOverlap);
			}
		}
	}

  // Finalize the column pointers of the sparse matrices R, W and Y
  m_R.setFromTriplets(Rvals.begin(), Rvals.end());
  m_R.makeCompressed();

  m_nonzeropivots = m_R.cols();	// Assuming all cols are nonzero

  m_isInitialized = true; 
  m_factorizationIsok = true;
  m_info = Success;
}

#define FULL_TO_BLOCK_VEC(fullVec, blockVec, yRows, yCols, blockRowIdx, blockNumZeros, numSubdiagElems) \
	blockVec = VectorXd(yRows); \
	blockVec.segment(0, yCols) = fullVec.segment(blockRowIdx, yCols); \
	if(numSubdiagElems > 0) { \
		blockVec.segment(yCols, numSubdiagElems) = fullVec.segment(blockRowIdx + yCols + blockNumZeros, numSubdiagElems); \
	} 

#define BLOCK_VEC_TO_FULL(fullVec, blockVec, yRows, yCols, blockRowIdx, blockNumZeros, numSubdiagElems) \
	fullVec.segment(blockRowIdx, yCols) = blockVec.segment(0, yCols); \
	fullVec.segment(blockRowIdx + yCols + blockNumZeros, numSubdiagElems) = blockVec.segment(yCols, numSubdiagElems);


// xxawf boilerplate all this into BlockSparseBandedBlockedQR...
template <typename SparseBandedBlockedQRType, typename Derived>
struct SparseBandedBlockedQR_QProduct : ReturnByValue<SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived> >
{
  typedef typename SparseBandedBlockedQRType::MatrixType MatrixType;
  typedef typename SparseBandedBlockedQRType::Scalar Scalar;

  typedef typename internal::SparseBandedBlockedQR_traits<MatrixType>::Vector SparseVector;

  // Get the references 
  SparseBandedBlockedQR_QProduct(const SparseBandedBlockedQRType& qr, const Derived& other, bool transpose) : 
  m_qr(qr),m_other(other),m_transpose(transpose) {}
  inline Index rows() const { return m_transpose ? m_qr.rows() : m_qr.cols(); }
  inline Index cols() const { return m_other.cols(); }

  // Assign to a vector
  template<typename DesType>
  void evalTo(DesType& res) const
  {
	  Index m = m_qr.rows();
	  Index n = m_qr.cols();

	  clock_t begin = clock();
	  
	  if(m_qr.m_useMultiThreading) {
		  /********************************* MT *****************************/

		  std::vector<std::vector<std::pair<typename MatrixType::Index, Scalar>>> resVals(m_other.cols());
		  Index numNonZeros = 0;

		  if (m_transpose)
		  {
			  // Compute res = Q' * other column by column using parallel for loop
			  const size_t nloop = m_other.cols();
			  const size_t nthreads = std::thread::hardware_concurrency();
			  {
				  std::vector<std::thread> threads(nthreads);
				  std::mutex critical;
				  for (int t = 0; t<nthreads; t++)
				  {
					  threads[t] = std::thread(std::bind(
						  [&](const int bi, const int ei, const int t)
					  {
						  // loop over all items
						  for (int j = bi; j<ei; j++)
						  {
							  // inner loop
							  {
								  VectorXd tmpResColJ;
								  SparseVector resColJ;
								  VectorXd resColJd;
								  resColJd = m_other.col(j).toDense();
								  for (Index k = 0; k < m_qr.m_blocksYT.size(); k++) {
									  MatrixType::StorageIndex subdiagElems = m_qr.m_blocksYT[k].value.rows() - m_qr.m_blocksYT[k].value.cols();
									  FULL_TO_BLOCK_VEC(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)

									  // We can afford noalias() in this case
									  tmpResColJ.noalias() += m_qr.m_blocksYT[k].value.multTransposed(tmpResColJ);

									  BLOCK_VEC_TO_FULL(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
								  }

								  std::lock_guard<std::mutex> lock(critical);
								  // Write the result back to j-th column of res
								  resColJ = resColJd.sparseView();
								  numNonZeros += resColJ.nonZeros();
								  resVals[j].reserve(resColJ.nonZeros());
								  for (SparseVector::InnerIterator it(resColJ); it; ++it) {
									  resVals[j].push_back(std::make_pair(it.row(), it.value()));
								  }

							  }
						  }
					  }, t*nloop / nthreads, (t + 1) == nthreads ? nloop : (t + 1)*nloop / nthreads, t));
				  }
				  std::for_each(threads.begin(), threads.end(), [](std::thread& x) {x.join(); });
			  }
		  }
		  else {
			  const size_t nloop = m_other.cols();
			  const size_t nthreads = std::thread::hardware_concurrency();
			  {
				  std::vector<std::thread> threads(nthreads);
				  std::mutex critical;
				  for (int t = 0; t < nthreads; t++)
				  {
					  threads[t] = std::thread(std::bind(
						  [&](const int bi, const int ei, const int t)
					  {
						  // loop over all items
						  for (int j = bi; j < ei; j++)
						  {
							  // inner loop
							  {
								  VectorXd tmpResColJ;
								  SparseVector resColJ;
								  VectorXd resColJd;
								  resColJd = m_other.col(j).toDense();
								  for (Index k = m_qr.m_blocksYT.size() - 1; k >= 0; k--) {
									  MatrixType::StorageIndex subdiagElems = m_qr.m_blocksYT[k].value.rows() - m_qr.m_blocksYT[k].value.cols();
									  FULL_TO_BLOCK_VEC(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
							
									  // We can afford noalias() in this case
									  tmpResColJ.noalias() += m_qr.m_blocksYT[k].value * tmpResColJ;
								 
									  BLOCK_VEC_TO_FULL(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
								  }

								  std::lock_guard<std::mutex> lock(critical);
								  // Write the result back to j-th column of res
								  resColJ = resColJd.sparseView();
								  numNonZeros += resColJ.nonZeros();
								  resVals[j].reserve(resColJ.nonZeros());
								  for (SparseVector::InnerIterator it(resColJ); it; ++it) {
									  resVals[j].push_back(std::make_pair(it.row(), it.value()));
								  }

							  }
						  }
					  }, t*nloop / nthreads, (t + 1) == nthreads ? nloop : (t + 1)*nloop / nthreads, t));
				  }
				  std::for_each(threads.begin(), threads.end(), [](std::thread& x) {x.join(); });
			  }
		
			}

			// Form the output
			res = Derived(m_other.rows(), m_other.cols());
			res.reserve(numNonZeros);
			for (int j = 0; j < resVals.size(); j++) {
				res.startVec(j);
				for (auto it = resVals[j].begin(); it != resVals[j].end(); ++it) {
					res.insertBack(it->first, j) = it->second;
				}
			}
			res.finalize();

		} else {
			/********************************* ST *****************************/
			res = Derived(m_other.rows(), m_other.cols());
			res.reserve(m_other.rows() * m_other.cols() * 0.25);// FixMe: Better estimation of nonzeros?

			if (m_transpose)
			{
				//Compute res = Q' * other column by column
				SparseVector resColJ;
				VectorXd resColJd;
				VectorXd tmpResColJ;
				for (Index j = 0; j < m_other.cols(); j++) {
					// Use temporary vector resColJ inside of the for loop - faster access
					resColJd = m_other.col(j).toDense();
					for (Index k = 0; k < m_qr.m_blocksYT.size(); k++) {
						MatrixType::StorageIndex subdiagElems = m_qr.m_blocksYT[k].value.rows() - m_qr.m_blocksYT[k].value.cols();
						FULL_TO_BLOCK_VEC(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)

						// We can afford noalias() in this case
						tmpResColJ.noalias() += m_qr.m_blocksYT[k].value.multTransposed(tmpResColJ);
					
						BLOCK_VEC_TO_FULL(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
					}
					// Write the result back to j-th column of res
					resColJ = resColJd.sparseView();
					res.startVec(j);
					for (SparseVector::InnerIterator it(resColJ); it; ++it) {
						res.insertBack(it.row(), j) = it.value();
					}
				}
			}
			else
			{
				// Compute res = Q * other column by column
				SparseVector resColJ;
				VectorXd resColJd;
				VectorXd tmpResColJ;
				for (Index j = 0; j < m_other.cols(); j++) {
					resColJd = m_other.col(j).toDense();
					for (Index k = m_qr.m_blocksYT.size() - 1; k >= 0; k--) {
						MatrixType::StorageIndex subdiagElems = m_qr.m_blocksYT[k].value.rows() - m_qr.m_blocksYT[k].value.cols();
						FULL_TO_BLOCK_VEC(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)

						// We can afford noalias() in this case
						tmpResColJ.noalias() += m_qr.m_blocksYT[k].value * tmpResColJ;
					
						BLOCK_VEC_TO_FULL(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
					}

					// Write the result back to j-th column of res
					resColJ = resColJd.sparseView();
					res.startVec(j);
					for (SparseVector::InnerIterator it(resColJ); it; ++it) {
						res.insertBack(it.row(), j) = it.value();
					}
				}
			}

			// Don't forget to call finalize
			res.finalize();
		}

	}

  const SparseBandedBlockedQRType& m_qr;
  const Derived& m_other;
  bool m_transpose;
};

template <typename SparseBandedBlockedQRType>
struct SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, VectorX> : ReturnByValue<SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, VectorX> >
{
	typedef typename SparseBandedBlockedQRType::MatrixType MatrixType;
	typedef typename SparseBandedBlockedQRType::Scalar Scalar;

	// Get the references 
	SparseBandedBlockedQR_QProduct(const SparseBandedBlockedQRType& qr, const VectorX& other, bool transpose) :
		m_qr(qr), m_other(other), m_transpose(transpose) {}
	inline Index rows() const { return m_transpose ? m_qr.rows() : m_qr.cols(); }
	inline Index cols() const { return m_other.cols(); }

	// Assign to a vector
	template<typename DesType>
	void evalTo(DesType& res) const
	{
		Index m = m_qr.rows();
		Index n = m_qr.cols();
		res = m_other;

		clock_t begin = clock();

		VectorX resTmp(res.rows() * res.cols());

		if (m_transpose)
		{
			//Compute res = Q' * other column by column
			VectorX resColJd;
			VectorX tmpResColJ;
			for (Index j = 0; j < res.cols(); j++) {
				// Use temporary vector resColJ inside of the for loop - faster access
				resColJd = res.col(j);
				for (Index k = 0; k < m_qr.m_blocksYT.size(); k++) {
					MatrixType::StorageIndex subdiagElems = m_qr.m_blocksYT[k].value.rows() - m_qr.m_blocksYT[k].value.cols();
					FULL_TO_BLOCK_VEC(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
					
					// We can afford noalias() in this case
					tmpResColJ.noalias() += m_qr.m_blocksYT[k].value.multTransposed(tmpResColJ);

					BLOCK_VEC_TO_FULL(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
				}

				resTmp.col(j) = resColJd;
			}
		}
		else
		{
			//begin = clock();
			// Compute res = Q * other column by column
			VectorX resColJd;
			VectorX tmpResColJ;
			for (Index j = 0; j < res.cols(); j++) {
				resColJd = res.col(j);
				for (Index k = m_qr.m_blocksYT.size() - 1; k >= 0; k--) {
					MatrixType::StorageIndex subdiagElems = m_qr.m_blocksYT[k].value.rows() - m_qr.m_blocksYT[k].value.cols();
					FULL_TO_BLOCK_VEC(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)

					// We can afford noalias() in this case
					tmpResColJ.noalias() += m_qr.m_blocksYT[k].value * tmpResColJ;

					BLOCK_VEC_TO_FULL(resColJd, tmpResColJ, m_qr.m_blocksYT[k].value.rows(), m_qr.m_blocksYT[k].value.cols(), m_qr.m_blocksYT[k].row, m_qr.m_blocksYT[k].value.numZeros(), subdiagElems)
				}

				resTmp.col(j) = resColJd;
			}
		}

		//std::cout << "Elapsed for loop: " << double(clock() - begin) / CLOCKS_PER_SEC << "s\n";

		// Assign the output
		res = resTmp;
	}

	const SparseBandedBlockedQRType& m_qr;
	const VectorX& m_other;
	bool m_transpose;
};

template<typename SparseBandedBlockedQRType>
struct SparseBandedBlockedQRMatrixQReturnType : public EigenBase<SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType> >
{  
  typedef typename SparseBandedBlockedQRType::Scalar Scalar;
  typedef Matrix<Scalar,Dynamic,Dynamic> DenseMatrix;
  enum {
    RowsAtCompileTime = Dynamic,
    ColsAtCompileTime = Dynamic
  };
  explicit SparseBandedBlockedQRMatrixQReturnType(const SparseBandedBlockedQRType& qr) : m_qr(qr) {}
  /*SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived> operator*(const MatrixBase<Derived>& other)
  {
    return SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType,Derived>(m_qr,other.derived(),false);
  }*/
  template<typename Derived>
  SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived> operator*(const MatrixBase<Derived>& other)
  {
	  return SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived>(m_qr, other.derived(), false);
  }
  template<typename _Scalar, int _Options, typename _Index>
  SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, SparseMatrix<_Scalar, _Options, _Index>> operator*(const SparseMatrix<_Scalar, _Options, _Index>& other)
  {
    return SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, SparseMatrix<_Scalar, _Options, _Index>>(m_qr, other, false);
  }
  SparseBandedBlockedQRMatrixQTransposeReturnType<SparseBandedBlockedQRType> adjoint() const
  {
    return SparseBandedBlockedQRMatrixQTransposeReturnType<SparseBandedBlockedQRType>(m_qr);
  }
  inline Index rows() const { return m_qr.rows(); }
  inline Index cols() const { return m_qr.rows(); }
  // To use for operations with the transpose of Q
  SparseBandedBlockedQRMatrixQTransposeReturnType<SparseBandedBlockedQRType> transpose() const
  {
    return SparseBandedBlockedQRMatrixQTransposeReturnType<SparseBandedBlockedQRType>(m_qr);
  }

  const SparseBandedBlockedQRType& m_qr;
};

template<typename SparseBandedBlockedQRType>
struct SparseBandedBlockedQRMatrixQTransposeReturnType
{
  explicit SparseBandedBlockedQRMatrixQTransposeReturnType(const SparseBandedBlockedQRType& qr) : m_qr(qr) {}
  template<typename Derived>
  SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived> operator*(const MatrixBase<Derived>& other)
  {
    return SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, Derived>(m_qr, other.derived(), true);
  }
  template<typename _Scalar, int _Options, typename _Index>
  SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, SparseMatrix<_Scalar, _Options, _Index>> operator*(const SparseMatrix<_Scalar,_Options,_Index>& other)
  {
    return SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, SparseMatrix<_Scalar, _Options, _Index>>(m_qr, other, true);
  }
  const SparseBandedBlockedQRType& m_qr;
};

namespace internal {
  
template<typename SparseBandedBlockedQRType>
struct evaluator_traits<SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType> >
{
  typedef typename SparseBandedBlockedQRType::MatrixType MatrixType;
  typedef typename storage_kind_to_evaluator_kind<typename MatrixType::StorageKind>::Kind Kind;
  typedef SparseShape Shape;
};

template< typename DstXprType, typename SparseBandedBlockedQRType>
struct Assignment<DstXprType, SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType>, internal::assign_op<typename DstXprType::Scalar,typename DstXprType::Scalar>, Sparse2Sparse>
{
  typedef SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType> SrcXprType;
  typedef typename DstXprType::Scalar Scalar;
  typedef typename DstXprType::StorageIndex StorageIndex;
  static void run(DstXprType &dst, const SrcXprType &src, const internal::assign_op<Scalar,Scalar> &/*func*/)
  {
    typename DstXprType::PlainObject idMat(src.m_qr.rows(), src.m_qr.rows());
    idMat.setIdentity();
    // Sort the sparse householder reflectors if needed
    //const_cast<SparseBandedBlockedQRType *>(&src.m_qr)->_sort_matrix_Q();
    dst = SparseBandedBlockedQR_QProduct<SparseBandedBlockedQRType, DstXprType>(src.m_qr, idMat, false);
  }
};

template< typename DstXprType, typename SparseBandedBlockedQRType>
struct Assignment<DstXprType, SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType>, internal::assign_op<typename DstXprType::Scalar,typename DstXprType::Scalar>, Sparse2Dense>
{
  typedef SparseBandedBlockedQRMatrixQReturnType<SparseBandedBlockedQRType> SrcXprType;
  typedef typename DstXprType::Scalar Scalar;
  typedef typename DstXprType::StorageIndex StorageIndex;
  static void run(DstXprType &dst, const SrcXprType &src, const internal::assign_op<Scalar,Scalar> &/*func*/)
  {
    dst = src.m_qr.matrixQ() * DstXprType::Identity(src.m_qr.rows(), src.m_qr.rows());
  }
};

} // end namespace internal

} // end namespace Eigen

#endif
