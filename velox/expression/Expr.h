/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <vector>

#include <folly/container/F14Map.h>

#include "velox/common/time/CpuWallTimer.h"
#include "velox/core/ExpressionEvaluator.h"
#include "velox/expression/EvalCtx.h"
#include "velox/expression/ExprStats.h"
#include "velox/expression/VectorFunction.h"
#include "velox/type/Subfield.h"
#include "velox/vector/SimpleVector.h"

namespace facebook::velox::exec {

class ExprSet;
class FieldReference;
class VectorFunction;

/// Maintains a set of rows for evaluation and removes rows with
/// nulls or errors as needed. Helps to avoid copying SelectivityVector in cases
/// when evaluation doesn't encounter nulls or errors.
class MutableRemainingRows {
 public:
  /// @param rows Initial set of rows.
  MutableRemainingRows(const SelectivityVector& rows, EvalCtx& context)
      : context_{context},
        originalRows_{&rows},
        rows_{&rows},
        mutableRowsHolder_{context} {}

  const SelectivityVector& originalRows() const {
    return *originalRows_;
  }

  /// @return current set of rows which may be different from the initial set if
  /// deselectNulls or deselectErrors were called.
  const SelectivityVector& rows() const {
    return *rows_;
  }

  SelectivityVector& mutableRows() {
    ensureMutableRemainingRows();
    return *mutableRows_;
  }

  /// Removes rows with nulls.
  /// @return true if at least one row remains.
  bool deselectNulls(const uint64_t* rawNulls) {
    ensureMutableRemainingRows();
    mutableRows_->deselectNulls(rawNulls, rows_->begin(), rows_->end());

    return mutableRows_->hasSelections();
  }

  /// Removes rows with errors (as recorded in EvalCtx::errors).
  /// @return true if at least one row remains.
  bool deselectErrors() {
    ensureMutableRemainingRows();
    context_.deselectErrors(*mutableRows_);

    return mutableRows_->hasSelections();
  }

  /// @return true if current set of rows is different from the original
  /// set of rows, which may happen if deselectNull() or deselectErrors() were
  /// called.
  bool hasChanged() const {
    return mutableRows_ != nullptr &&
        mutableRows_->countSelected() != originalRows_->countSelected();
  }

 private:
  void ensureMutableRemainingRows() {
    if (mutableRows_ == nullptr) {
      mutableRows_ = mutableRowsHolder_.get(*rows_);
      rows_ = mutableRows_;
    }
  }

  EvalCtx& context_;
  const SelectivityVector* const originalRows_;

  const SelectivityVector* rows_;

  SelectivityVector* mutableRows_{nullptr};
  LocalSelectivityVector mutableRowsHolder_;
};

enum class SpecialFormKind : int32_t {
  kFieldAccess = 0,
  kConstant = 1,
  kCast = 2,
  kCoalesce = 3,
  kSwitch = 4,
  kLambda = 5,
  kTry = 6,
  kAnd = 7,
  kOr = 8,
  kCustom = 999,
};

VELOX_DECLARE_ENUM_NAME(SpecialFormKind);

// An executable expression.
class Expr {
 public:
  Expr(
      TypePtr type,
      std::vector<std::shared_ptr<Expr>>&& inputs,
      std::string name,
      std::optional<SpecialFormKind> specialFormKind,
      bool supportsFlatNoNullsFastPath,
      bool trackCpuUsage)
      : type_(std::move(type)),
        inputs_(std::move(inputs)),
        name_(std::move(name)),
        vectorFunction_(nullptr),
        specialFormKind_{specialFormKind},
        supportsFlatNoNullsFastPath_{supportsFlatNoNullsFastPath},
        trackCpuUsage_{trackCpuUsage} {}

  Expr(
      TypePtr type,
      std::vector<std::shared_ptr<Expr>>&& inputs,
      std::shared_ptr<VectorFunction> vectorFunction,
      VectorFunctionMetadata metadata,
      std::string name,
      bool trackCpuUsage);

  virtual ~Expr() = default;

  /// Evaluates the expression for the specified 'rows'.
  ///
  /// @param parentExprSet pointer to the parent ExprSet which is calling
  /// evaluate on this expression. Should only be set for top level expressions
  /// and not passed on to child expressions as it is ssed to setup exception
  /// context.
  void eval(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result,
      const ExprSet* parentExprSet = nullptr);

  /// Evaluates the expression using fast path that assumes all inputs and
  /// intermediate results are flat or constant and have no nulls.
  ///
  /// This path doesn't peel off constant encoding and therefore may be
  /// expensive to apply to expressions that manipulate strings of complex
  /// types. It may also be expensive to apply to large batches. Hence, this
  /// path is enabled only for batch sizes less than 1'000 and expressions where
  /// all input and intermediate types are primitive and not strings.
  ///
  /// @param parentExprSet pointer to the parent ExprSet which is calling
  /// evaluate on this expression. Should only be set for top level expressions
  /// and not passed on to child expressions as it is ssed to setup exception
  /// context.
  void evalFlatNoNulls(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result,
      const ExprSet* parentExprSet = nullptr);

  void evalFlatNoNullsImpl(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result,
      const ExprSet* parentExprSet);

  // Simplified path for expression evaluation (flattens all vectors).
  void evalSimplified(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  // Evaluates 'this', including inputs. This is defined only for
  // exprs that have custom error handling or evaluate their arguments
  // conditionally.
  virtual void evalSpecialForm(
      const SelectivityVector& /*rows*/,
      EvalCtx& /*context*/,
      VectorPtr& /*result*/) {
    VELOX_NYI();
  }

  // Allow special form expressions to overwrite and implement a simplified
  // path; fallback to the regular implementation by default.
  virtual void evalSpecialFormSimplified(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result) {
    evalSpecialForm(rows, context, result);
  }

  // Return true if the current function is deterministic, regardless of the
  // determinism of its inputs. Return false otherwise. Note that this is
  // different from deterministic_ that represents the determinism of the
  // current expression including its inputs.
  bool isCurrentFunctionDeterministic() const;

  // Compute the following properties: deterministic_, propagatesNulls_,
  // distinctFields_, multiplyReferencedFields_, hasConditionals_ and
  // sameAsParentDistinctFields_.
  void computeMetadata();

  // Utility function to add fields to both distinct and multiply referenced
  // fields.
  static void mergeFields(
      std::vector<FieldReference*>& distinctFields,
      std::unordered_set<FieldReference*>& multiplyReferencedFields,
      const std::vector<FieldReference*>& fieldsToAdd);

  virtual void reset() {
    sharedSubexprResults_.clear();
  }

  void clearMemo() {
    baseOfDictionaryRepeats_ = 0;
    baseOfDictionaryRawPtr_ = nullptr;
    baseOfDictionaryWeakPtr_.reset();
    baseOfDictionary_.reset();
    dictionaryCache_ = nullptr;
    cachedDictionaryIndices_ = nullptr;
  }

  virtual void clearCache() {
    sharedSubexprResults_.clear();
    clearMemo();
    for (auto& input : inputs_) {
      input->clearCache();
    }
  }

  const TypePtr& type() const {
    return type_;
  }

  const std::string& name() const {
    return name_;
  }

  bool isString() const {
    return type()->kind() == TypeKind::VARCHAR;
  }

  bool isSpecialForm() const {
    return specialFormKind_.has_value();
  }

  SpecialFormKind specialFormKind() const {
    return specialFormKind_.value();
  }

  bool isFieldAccess() const {
    return specialFormKind_ == SpecialFormKind::kFieldAccess;
  }

  bool isConstant() const {
    return specialFormKind_ == SpecialFormKind::kConstant;
  }

  bool isCast() const {
    return specialFormKind_ == SpecialFormKind::kCast;
  }

  bool isCoalesce() const {
    return specialFormKind_ == SpecialFormKind::kCoalesce;
  }

  bool isSwitch() const {
    return specialFormKind_ == SpecialFormKind::kSwitch;
  }

  bool isLambda() const {
    return specialFormKind_ == SpecialFormKind::kLambda;
  }

  bool isTry() const {
    return specialFormKind_ == SpecialFormKind::kTry;
  }

  bool isAnd() const {
    return specialFormKind_ == SpecialFormKind::kAnd;
  }

  bool isOr() const {
    return specialFormKind_ == SpecialFormKind::kOr;
  }

  bool isCustom() const {
    return specialFormKind_ == SpecialFormKind::kCustom;
  }

  virtual bool isConditional() const {
    return false;
  }

  bool hasConditionals() const {
    return hasConditionals_;
  }

  bool isDeterministic() const {
    return deterministic_;
  }

  virtual bool isConstantExpr() const;

  bool supportsFlatNoNullsFastPath() const {
    return supportsFlatNoNullsFastPath_;
  }

  bool isMultiplyReferenced() const {
    return isMultiplyReferenced_;
  }

  void setMultiplyReferenced() {
    isMultiplyReferenced_ = true;
  }

  /// True if this is a special form where the next argument will always be
  /// evaluated on a subset of the rows for which the previous one was
  /// evaluated.  This is true of AND and no other at this time.  This implies
  /// that lazies can be loaded on first use and not before starting evaluating
  /// the form.  This is so because a subsequent use will never access rows that
  /// were not in scope for the previous one.
  virtual bool evaluatesArgumentsOnNonIncreasingSelection() const {
    return false;
  }

  std::vector<common::Subfield> extractSubfields() const;

  virtual void extractSubfieldsImpl(
      folly::F14FastMap<std::string, int32_t>* shadowedNames,
      std::vector<common::Subfield>* subfields) const;

  template <typename T>
  const T* as() const {
    return dynamic_cast<const T*>(this);
  }

  template <typename T>
  T* as() {
    return dynamic_cast<T*>(this);
  }

  template <typename T>
  bool is() const {
    return as<T>() != nullptr;
  }

  // True if 'this' Expr tree is null for a null in any of the columns
  // this depends on.
  bool propagatesNulls() const {
    return propagatesNulls_;
  }

  const std::vector<FieldReference*>& distinctFields() const {
    return distinctFields_;
  }

  static bool isSameFields(
      const std::vector<FieldReference*>& fields1,
      const std::vector<FieldReference*>& fields2);

  static bool isSubsetOfFields(
      const std::vector<FieldReference*>& subset,
      const std::vector<FieldReference*>& superset);

  static bool allSupportFlatNoNullsFastPath(
      const std::vector<std::shared_ptr<Expr>>& exprs);

  const std::vector<std::shared_ptr<Expr>>& inputs() const {
    return inputs_;
  }

  /// @param recursive If true, the output includes input expressions and all
  /// their inputs recursively.
  virtual std::string toString(bool recursive = true) const;

  /// Return the expression as SQL string.
  /// @param complexConstants An optional std::vector of VectorPtr to record
  /// complex constants (Array, Maps, Structs, ...) that aren't accurately
  /// expressable as sql. If not given, they will be converted to
  /// SQL-expressable simple constants.
  virtual std::string toSql(
      std::vector<VectorPtr>* complexConstants = nullptr) const;

  const ExprStats& stats() const {
    return stats_;
  }

  void addNulls(
      const SelectivityVector& rows,
      const uint64_t* rawNulls,
      EvalCtx& context,
      VectorPtr& result) const;

  const std::shared_ptr<VectorFunction>& vectorFunction() const {
    return vectorFunction_;
  }

  const VectorFunctionMetadata& vectorFunctionMetadata() const {
    return vectorFunctionMetadata_;
  }

  std::vector<VectorPtr>& inputValues() {
    return inputValues_;
  }

  void setAllNulls(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result) const;

  void clearMetaData();

  // No need to peel encoding or remove sure nulls for default null propagating
  // expressions when the expression has single parent(the expression that
  // reference it) and have the same distinct fields as its parent.
  // The reason is because such optimizations would be redundant in that case,
  // since they would have been performed identically on the parent.
  bool skipFieldDependentOptimizations() const {
    if (!isMultiplyReferenced_ && sameAsParentDistinctFields_) {
      return true;
    }
    if (distinctFields_.empty()) {
      return true;
    }
    return false;
  }

 private:
  struct PeelEncodingsResult {
    SelectivityVector* newRows;
    SelectivityVector* newFinalSelection;
    bool mayCache;

    static PeelEncodingsResult empty() {
      return {nullptr, nullptr, false};
    }
  };

  PeelEncodingsResult peelEncodings(
      EvalCtx& context,
      ContextSaver& saver,
      const SelectivityVector& rows,
      LocalDecodedVector& localDecoded,
      LocalSelectivityVector& newRowsHolder,
      LocalSelectivityVector& finalRowsHolder);

  void evalEncodings(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  void evalWithMemo(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  void evalWithNulls(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  void
  evalAll(const SelectivityVector& rows, EvalCtx& context, VectorPtr& result);

  void evalAllImpl(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  // Checks 'inputValues_' for peelable wrappers (constants,
  // dictionaries etc) and applies the function of 'this' to distinct
  // values as opposed to all values. Wraps the return value into a
  // dictionary or constant so that we get the right
  // cardinality. Returns true if the function was called. Returns
  // false if no encodings could be peeled off.
  bool applyFunctionWithPeeling(
      const SelectivityVector& applyRows,
      EvalCtx& context,
      VectorPtr& result);

  // Calls the function of 'this' on arguments in
  // 'inputValues_'. Handles cases of VectorFunction and SimpleFunction.
  void applyFunction(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  // Returns true if values in 'distinctFields_' have nulls that are
  // worth skipping. If so, the rows in 'rows' with at least one sure
  // null are deselected in 'nullHolder->get()'.
  bool removeSureNulls(
      const SelectivityVector& rows,
      EvalCtx& context,
      LocalSelectivityVector& nullHolder);

  /// Returns true if this is a deterministic shared sub-expressions with at
  /// least one input (i.e. not a constant or field access expression).
  /// Evaluation of such expression is optimized by memoizing and reusing
  /// the results of prior evaluations. That logic is implemented in
  /// 'evaluateSharedSubexpr'.
  bool shouldEvaluateSharedSubexp(EvalCtx& context) const {
    return deterministic_ && isMultiplyReferenced_ && !inputs_.empty() &&
        context.sharedSubExpressionReuseEnabled();
  }

  /// Evaluate common sub-expression. Check if sharedSubexprValues_ already has
  /// values for all 'rows'. If not, compute missing values.
  template <typename TEval>
  void evaluateSharedSubexpr(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result,
      TEval eval);

  /// Return true if errors in evaluation 'vectorFunction_' arguments should be
  /// thrown as soon as they happen. False if argument errors will be converted
  /// into a null if another argument for the same row is null.
  bool throwArgumentErrors(const EvalCtx& context) const;

  void evalSimplifiedImpl(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

  void evalSpecialFormWithStats(
      const SelectivityVector& rows,
      EvalCtx& context,
      VectorPtr& result);

 protected:
  void appendInputs(std::stringstream& stream) const;

  void appendInputsSql(
      std::stringstream& stream,
      std::vector<VectorPtr>* complexConstants) const;

  /// Release 'inputValues_' back to vector pool in 'evalCtx' so they can be
  /// reused.
  void releaseInputValues(EvalCtx& evalCtx);

  // Evaluates arguments of 'this' for 'rows'. 'rows' is updated to
  // remove rows where an argument is null. If an argument gets an
  // error and a subsequent argument has a null for the same row the
  // error is masked and the row is removed from 'rows'. If
  // 'context.throwOnError()' is true, an error in arguments that is
  // not cancelled by a null will be thrown. Otherwise the errors
  // found in arguments are left in place and added to errors that may
  // have been in 'context' on entry. The rows where an argument had a
  // null or error are removed from 'rows' at before returning. If
  // 'rows' goes empty, we return false as soon as 'rows' is empty and set
  // 'result' to nulls for initial 'rows'.. 'evalArg(i)' is called for
  // evaluating the 'ith' argument.
  template <typename EvalArg>
  bool evalArgsDefaultNulls(
      MutableRemainingRows& rows,
      EvalArg evalArg,
      EvalCtx& context,
      VectorPtr& result);

  // Evaluates arguments of 'this'. A null or does not remove its row
  // from 'rows'. If 'context.throwOnError()' is false, errors are
  // accumulated in 'context' adding themselves or overwriting
  // previous errors for the same row. An error removes the
  // corresponding row from 'rows.  Returns false if 'rows' goes
  // empty and sets 'result'  to null for initial 'rows'. . 'evalArgs(i)' is
  // called for evaluating the 'ith' argument.
  template <typename EvalArg>
  bool evalArgsWithNulls(
      MutableRemainingRows& rows,
      EvalArg evalArg,
      EvalCtx& context,
      VectorPtr& result);

  /// Returns an instance of CpuWallTimer if cpu usage tracking is enabled. Null
  /// otherwise.
  std::unique_ptr<CpuWallTimer> cpuWallTimer() {
    return trackCpuUsage_ ? std::make_unique<CpuWallTimer>(stats_.timing)
                          : nullptr;
  }

  // Should be called only after computeMetadata() has been called on 'inputs_'.
  // Computes distinctFields for this expression. Also updates any multiply
  // referenced fields.
  virtual void computeDistinctFields();

  const TypePtr type_;
  const std::vector<std::shared_ptr<Expr>> inputs_;
  const std::string name_;
  const std::shared_ptr<VectorFunction> vectorFunction_;
  const VectorFunctionMetadata vectorFunctionMetadata_;
  const std::optional<SpecialFormKind> specialFormKind_;
  const bool supportsFlatNoNullsFastPath_;
  const bool trackCpuUsage_;

  std::vector<VectorPtr> constantInputs_;
  std::vector<bool> inputIsConstant_;

  // TODO make the following metadata const, e.g. call computeMetadata in the
  // constructor

  // The distinct references to input columns in 'inputs_'
  // subtrees. Empty if this is the same as 'distinctFields_' of
  // parent Expr.
  std::vector<FieldReference*> distinctFields_;

  // Fields referenced by multiple inputs, which is subset of distinctFields_.
  // Used to determine pre-loading of lazy vectors at current expr.
  std::unordered_set<FieldReference*> multiplyReferencedFields_;

  // True if a null in any of 'distinctFields_' causes 'this' to be
  // null for the row.
  bool propagatesNulls_ = false;

  // True if this and all children are deterministic.
  bool deterministic_ = true;

  // True if this or a sub-expression is an IF, AND or OR.
  bool hasConditionals_ = false;

  bool isMultiplyReferenced_ = false;

  std::vector<VectorPtr> inputValues_;

  /// Represents a set of inputs referenced by 'distinctFields_' that are
  /// captured when the 'evaluateSharedSubexpr()' method is called on a shared
  /// sub-expression. The purpose of this class is to ensure that cached
  /// results are re-used for the correct set of live input vectors.
  class InputForSharedResults {
   public:
    void addInput(const std::shared_ptr<BaseVector>& input) {
      inputVectors_.push_back(input.get());
      inputWeakVectors_.push_back(input);
    }

    bool operator<(const InputForSharedResults& other) const {
      return inputVectors_ < other.inputVectors_;
    }

    bool isExpired() const {
      for (const auto& input : inputWeakVectors_) {
        if (input.expired()) {
          return true;
        }
      }
      return false;
    }

   private:
    // Used as a key in a map that keeps track of cached results.
    std::vector<const BaseVector*> inputVectors_;
    // Used to check if inputs have expired.
    std::vector<std::weak_ptr<BaseVector>> inputWeakVectors_;
  };

  struct SharedResults {
    // The rows for which 'sharedSubexprValues_' has a value.
    std::unique_ptr<SelectivityVector> sharedSubexprRows_ = nullptr;
    // If multiply referenced or literal, these are the values.
    VectorPtr sharedSubexprValues_ = nullptr;
  };

  // Maps the inputs referenced by distinctFields_ captuered when
  // evaluateSharedSubexpr() is called to the cached shared results.
  std::map<InputForSharedResults, SharedResults> sharedSubexprResults_;

  // Pointers to the last base vector of cachable dictionary input. Used to
  // check if the current input's base vector is the same as the last. If it's
  // the same, then results can be cached.
  std::weak_ptr<BaseVector> baseOfDictionaryWeakPtr_;
  BaseVector* baseOfDictionaryRawPtr_ = nullptr;

  // This is a strong reference to the base vector and is only set if
  // `baseOfDictionaryRepeats_` > 1. This is to ensure that the vector held is
  // not modified and re-used in-place.
  VectorPtr baseOfDictionary_;

  // Number of times currently held cacheable vector is seen for a non-first
  // time. Is reset everytime 'baseOfDictionaryRawPtr_' is different from the
  // current input's base.
  int baseOfDictionaryRepeats_ = 0;

  // Values computed for the base dictionary, 1:1 to the positions in
  // 'baseOfDictionaryRawPtr_'.
  VectorPtr dictionaryCache_;

  // The indices that are valid in 'dictionaryCache_'.
  std::unique_ptr<SelectivityVector> cachedDictionaryIndices_;

  /// Runtime statistics. CPU time, wall time and number of processed rows.
  ExprStats stats_;

  // If true computeMetaData returns, otherwise meta data is computed and the
  // flag is set to true.
  bool metaDataComputed_ = false;

  // True if distinctFields_ are identical to at least one of the parent
  // expression's distinct fields.
  bool sameAsParentDistinctFields_ = false;
};

/// Generate a selectivity vector of a single row.
SelectivityVector* singleRow(LocalSelectivityVector& holder, vector_size_t row);

using ExprPtr = std::shared_ptr<Expr>;

// A set of Exprs that get evaluated together. Common subexpressions
// can be deduplicated. This is the top level handle on an expression
// and is used also if only one Expr is to be evaluated. TODO: Rename to
// ExprList.
// Note: Caller must ensure that lazy vectors associated with field references
// used by the expressions in this ExprSet are pre-loaded (before running
// evaluation on them) if they are also used/referenced outside the context of
// this ExprSet. If however such an association cannot be made with certainty,
// then its advisable to pre-load all lazy vectors to avoid issues associated
// with partial loading.
class ExprSet {
 public:
  explicit ExprSet(
      const std::vector<core::TypedExprPtr>& source,
      core::ExecCtx* execCtx,
      bool enableConstantFolding = true);

  virtual ~ExprSet();

  // Initialize and evaluate all expressions available in this ExprSet.
  void eval(
      const SelectivityVector& rows,
      EvalCtx& ctx,
      std::vector<VectorPtr>& result) {
    eval(0, exprs_.size(), true, rows, ctx, result);
  }

  // Evaluate from expression `begin` to `end`.
  virtual void eval(
      int32_t begin,
      int32_t end,
      bool initialize,
      const SelectivityVector& rows,
      EvalCtx& ctx,
      std::vector<VectorPtr>& result);

  void clear();

  /// Clears the internally cached buffers used for shared sub-expressions and
  /// dictionary memoization which are allocated through memory pool. This is
  /// used by memory arbitration to reclaim memory.
  void clearCache();

  core::ExecCtx* execCtx() const {
    return execCtx_;
  }

  auto size() const {
    return exprs_.size();
  }

  const std::vector<std::shared_ptr<Expr>>& exprs() const {
    return exprs_;
  }

  const std::shared_ptr<Expr>& expr(int32_t index) const {
    return exprs_[index];
  }

  const std::vector<FieldReference*>& distinctFields() const {
    return distinctFields_;
  }

  // Flags a shared subexpression which needs to be reset (e.g. previously
  // computed results must be deleted) when evaluating new batch of data.
  void addToReset(const std::shared_ptr<Expr>& expr) {
    toReset_.emplace_back(expr);
  }

  // Flags an expression that remembers the results for a dictionary.
  void addToMemo(Expr* expr) {
    memoizingExprs_.insert(expr);
  }

  /// Returns text representation of the expression set.
  /// @param compact If true, uses one-line representation for each expression.
  /// Otherwise, prints a tree of expressions one node per line.
  std::string toString(bool compact = true) const;

  /// Returns evaluation statistics as a map keyed on function or special form
  /// name. If a function or a special form occurs in the expression
  /// multiple times, the statistics will be aggregated across all calls.
  /// Statistics will be missing for functions and special forms that didn't get
  /// evaluated. If 'excludeSpecialForm' is true, special forms are excluded.
  std::unordered_map<std::string, exec::ExprStats> stats(
      bool excludeSpecialForm = false) const;

 protected:
  void clearSharedSubexprs();

  std::vector<std::shared_ptr<Expr>> exprs_;

  // The distinct references to input columns among all expressions in ExprSet.
  std::vector<FieldReference*> distinctFields_;

  // Fields referenced by multiple expressions in ExprSet.
  std::unordered_set<FieldReference*> multiplyReferencedFields_;

  // Distinct Exprs reachable from 'exprs_' for which reset() needs to
  // be called at the start of eval().
  std::vector<std::shared_ptr<Expr>> toReset_;

  // Exprs which retain memoized state, e.g. from running over dictionaries.
  std::unordered_set<Expr*> memoizingExprs_;
  core::ExecCtx* const execCtx_;
};

class ExprSetSimplified : public ExprSet {
 public:
  ExprSetSimplified(
      const std::vector<core::TypedExprPtr>& source,
      core::ExecCtx* execCtx)
      : ExprSet(source, execCtx, /*enableConstantFolding*/ false) {}

  virtual ~ExprSetSimplified() override {}

  // Initialize and evaluate all expressions available in this ExprSet.
  void eval(
      const SelectivityVector& rows,
      EvalCtx& ctx,
      std::vector<VectorPtr>& result) {
    eval(0, exprs_.size(), true, rows, ctx, result);
  }

  void eval(
      int32_t begin,
      int32_t end,
      bool initialize,
      const SelectivityVector& rows,
      EvalCtx& ctx,
      std::vector<VectorPtr>& result) override;
};

// Factory method that takes `kExprEvalSimplified` (query parameter) into
// account and instantiates the correct ExprSet class.
std::unique_ptr<ExprSet> makeExprSetFromFlag(
    std::vector<core::TypedExprPtr>&& source,
    core::ExecCtx* execCtx);

/// Evaluates a deterministic expression that doesn't depend on any inputs and
/// returns the result as single-row vector. Returns nullptr if the expression
/// is non-deterministic or has dependencies.
///
/// By default, propagates failures that occur during evaluation of the
/// expression. For example, evaluating 5 / 0 throws "division by zero". If
/// 'suppressEvaluationFailures' is true, these failures are swallowed and the
/// caller receives a nullptr result.
VectorPtr tryEvaluateConstantExpression(
    const core::TypedExprPtr& expr,
    memory::MemoryPool* pool,
    const std::shared_ptr<core::QueryCtx>& queryCtx,
    bool suppressEvaluationFailures = false);

/// Returns a string representation of the expression trees annotated with
/// runtime statistics. Expected to be called after calling ExprSet::eval one or
/// more times. If called before ExprSet::eval runtime statistics will be all
/// zeros.
std::string printExprWithStats(const ExprSet& exprSet);

struct ExprSetCompletionEvent {
  /// Aggregated runtime stats keyed on expression name (e.g. built-in
  /// expression like and, or, switch or a function name).
  std::unordered_map<std::string, exec::ExprStats> stats;
  /// List containing sql representation of each top level expression in ExprSet
  std::vector<std::string> sqls;
  // Query id corresponding query
  std::string queryId;
};

/// Listener invoked on ExprSet destruction.
class ExprSetListener {
 public:
  virtual ~ExprSetListener() = default;

  /// Called on ExprSet destruction. Provides runtime statistics about
  /// expression evaluation.
  /// @param uuid Universally unique identifier of the set of expressions.
  /// @param event Runtime stats.
  virtual void onCompletion(
      const std::string& uuid,
      const ExprSetCompletionEvent& event) = 0;

  /// Called when a batch of rows encounters errors processing one or more
  /// rows in a try expression to provide information about these errors.
  /// @param numRows Number of rows with errors.
  /// @param queryId Query ID.
  virtual void onError(vector_size_t numRows, const std::string& queryId) = 0;
};

/// Return the ExprSetListeners having been registered.
folly::Synchronized<std::vector<std::shared_ptr<ExprSetListener>>>&
exprSetListeners();

/// Register a listener to be invoked on ExprSet destruction. Returns true if
/// listener was successfully registered, false if listener is already
/// registered.
bool registerExprSetListener(std::shared_ptr<ExprSetListener> listener);

/// Unregister a listener registered earlier. Returns true if listener was
/// unregistered successfully, false if listener was not found.
bool unregisterExprSetListener(
    const std::shared_ptr<ExprSetListener>& listener);

class SimpleExpressionEvaluator : public core::ExpressionEvaluator {
 public:
  SimpleExpressionEvaluator(core::QueryCtx* queryCtx, memory::MemoryPool* pool)
      : queryCtx_(queryCtx), pool_(pool) {}

  std::unique_ptr<ExprSet> compile(
      const core::TypedExprPtr& expression) override {
    return std::make_unique<ExprSet>(
        std::vector<core::TypedExprPtr>{expression}, ensureExecCtx());
  }

  std::unique_ptr<ExprSet> compile(
      const std::vector<core::TypedExprPtr>& expressions) override {
    return std::make_unique<ExprSet>(expressions, ensureExecCtx());
  }

  void evaluate(
      ExprSet* exprSet,
      const SelectivityVector& rows,
      const RowVector& input,
      VectorPtr& result) override;

  void evaluate(
      exec::ExprSet* exprSet,
      const SelectivityVector& rows,
      const RowVector& input,
      std::vector<VectorPtr>& results) override;

  memory::MemoryPool* pool() override {
    return pool_;
  }

 private:
  core::ExecCtx* ensureExecCtx();

  core::QueryCtx* const queryCtx_;
  memory::MemoryPool* const pool_;
  std::unique_ptr<core::ExecCtx> execCtx_;
};

class Subscript : public exec::VectorFunction {
 public:
  virtual bool canPushdown() const {
    return false;
  }
};

} // namespace facebook::velox::exec
