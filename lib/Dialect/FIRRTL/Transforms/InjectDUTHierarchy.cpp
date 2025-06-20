//===- InjectDUTHierarchy.cpp - Add hierarchy above the DUT ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SiFive transform InjectDUTHierarchy.  This moves all
// the logic inside the DUT into a new module named using an annotation.
//
//===----------------------------------------------------------------------===//

#include "circt/Analysis/FIRRTLInstanceInfo.h"
#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/FIRRTL/NLATable.h"
#include "circt/Dialect/FIRRTL/Namespace.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/HW/InnerSymbolNamespace.h"
#include "circt/Support/Debug.h"
#include "mlir/Pass/Pass.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "firrtl-inject-dut-hier"

namespace circt {
namespace firrtl {
#define GEN_PASS_DEF_INJECTDUTHIERARCHY
#include "circt/Dialect/FIRRTL/Passes.h.inc"
} // namespace firrtl
} // namespace circt

using namespace circt;
using namespace firrtl;

namespace {
struct InjectDUTHierarchy
    : public circt::firrtl::impl::InjectDUTHierarchyBase<InjectDUTHierarchy> {
  void runOnOperation() override;
};
} // namespace

/// Add an extra level of hierarchy to a hierarchical path that places the
/// wrapper instance after the DUT.  E.g., this is converting:
///
///   firrtl.hierpath [@Top::@dut, @DUT]
///
/// Int:
///
///   firrtl.hierpath [@Top::@dut, @DUT::@wrapper, @Wrapper]
static void addHierarchy(hw::HierPathOp path, FModuleOp dut,
                         InstanceOp wrapperInst) {
  auto namepath = path.getNamepath().getValue();

  size_t nlaIdx = 0;
  SmallVector<Attribute> newNamepath;
  newNamepath.reserve(namepath.size() + 1);
  while (path.modPart(nlaIdx) != dut.getNameAttr())
    newNamepath.push_back(namepath[nlaIdx++]);
  newNamepath.push_back(hw::InnerRefAttr::get(dut.getModuleNameAttr(),
                                              getInnerSymName(wrapperInst)));

  // Add the extra level of hierarchy.
  if (auto dutRef = dyn_cast<hw::InnerRefAttr>(namepath[nlaIdx]))
    newNamepath.push_back(hw::InnerRefAttr::get(
        wrapperInst.getModuleNameAttr().getAttr(), dutRef.getName()));
  else
    newNamepath.push_back(
        FlatSymbolRefAttr::get(wrapperInst.getModuleNameAttr().getAttr()));

  // Add anything left over.
  auto back = namepath.drop_front(nlaIdx + 1);
  newNamepath.append(back.begin(), back.end());
  path.setNamepathAttr(ArrayAttr::get(dut.getContext(), newNamepath));
}

void InjectDUTHierarchy::runOnOperation() {
  LLVM_DEBUG(debugPassHeader(this) << "\n";);

  CircuitOp circuit = getOperation();

  /// The wrapper module that is created inside the DUT to house all its logic.
  FModuleOp wrapper;

  /// The name of the new module to create under the DUT.
  StringAttr wrapperName;

  /// If true, then move the MarkDUTAnnotation to the newly created module.
  bool moveDut = false;

  /// Mutable indicator that an error occurred for some reason.  If this is ever
  /// true, then the pass can just signalPassFailure.
  bool error = false;

  // Do not remove the injection annotation as this is necessary to additionally
  // influence ExtractInstances.
  for (Annotation anno : AnnotationSet(circuit)) {
    if (!anno.isClass(injectDUTHierarchyAnnoClass))
      continue;

    auto name = anno.getMember<StringAttr>("name");
    if (!name) {
      emitError(circuit->getLoc())
          << "contained a malformed "
             "'sifive.enterprise.firrtl.InjectDUTHierarchyAnnotation' "
             "annotation that did not contain a 'name' field";
      error = true;
      continue;
    }

    if (wrapperName) {
      emitError(circuit->getLoc())
          << "contained multiple "
             "'sifive.enterprise.firrtl.InjectDUTHierarchyAnnotation' "
             "annotations when at most one is allowed";
      error = true;
      continue;
    }

    wrapperName = name;
    if (auto moveDutAnnoAttr = anno.getMember<BoolAttr>("moveDut"))
      moveDut = moveDutAnnoAttr.getValue();
  }

  if (error)
    return signalPassFailure();

  // The prerequisites for the pass to run were not met.  Indicate that no work
  // was done and exit.
  if (!wrapperName)
    return markAllAnalysesPreserved();

  // A DUT must exist in order to continue.  The pass could silently ignore this
  // case and do nothing, but it is better to provide an error.
  InstanceInfo &instanceInfo = getAnalysis<InstanceInfo>();
  if (!instanceInfo.getDut()) {
    emitError(circuit->getLoc())
        << "contained a '" << injectDUTHierarchyAnnoClass << "', but no '"
        << dutAnnoClass << "' was provided";
    return signalPassFailure();
  }

  /// The design-under-test (DUT).  This is kept up-to-date by the pass as the
  /// DUT changes due to internal logic.
  FModuleOp dut = cast<FModuleOp>(instanceInfo.getDut());

  // Create a module that will become the new DUT.  The original DUT is renamed
  // to become the wrapper.  This is done to save copying into the wrapper.
  // While the logical movement is "copy the body of the DUT into a wrapper", it
  // is mechanically more straigthforward to make the DUT the wrappper.  After
  // this block finishes, the "dut" and "wrapper" variables are set correctly.
  // This logic is intentionally put into a block to avoid confusion while the
  // dut and wrapper do not match the logical definition.
  OpBuilder b(circuit.getContext());
  CircuitNamespace circuitNS(circuit);
  {
    b.setInsertionPointAfter(dut);
    auto newDUT = b.create<FModuleOp>(dut.getLoc(), dut.getNameAttr(),
                                      dut.getConventionAttr(), dut.getPorts(),
                                      dut.getAnnotationsAttr());

    // This pass shouldn't create new public modules.  It should only preserve
    // the existing public modules.  In "moveDut" mode, then the wrapper is the
    // new DUT and we should move the publicness from the old DUT to the
    // wrapper.  When not in "moveDut" mode, then the wrapper should be made
    // private.
    //
    // Note: `movedDut=true` violates the FIRRTL ABI unless the user it doing
    // something clever with module prefixing.  Because this annotation is
    // already outside the specification, this workflow is allowed even though
    // it violates the FIRRTL ABI.  The mid-term plan is to remove this pass to
    // avoid the tech debt that it creates.
    if (moveDut) {
      newDUT.setPrivate();
    } else {
      newDUT.setVisibility(dut.getVisibility());
      dut.setPrivate();
    }
    dut.setName(b.getStringAttr(circuitNS.newName(wrapperName.getValue())));

    // The original DUT module is now the wrapper.  The new module we just
    // created becomse the DUT.
    wrapper = dut;
    dut = newDUT;

    // Finish setting up the wrapper.  Strip the `MarkDUTAnnotation` if we are
    // in "moveDut" mode.
    AnnotationSet::removePortAnnotations(wrapper,
                                         [](auto, auto) { return true; });
    AnnotationSet::removeAnnotations(wrapper, [&](Annotation anno) {
      if (anno.isClass(dutAnnoClass))
        return !moveDut;
      return true;
    });

    // Finish setting up the DUT.  Strip the `MarkDUTAnnotation` is we are in
    // "moveDut" mode.
    if (moveDut)
      AnnotationSet::removeAnnotations(dut, dutAnnoClass);
  }

  // Instantiate the wrapper inside the DUT and wire it up.
  b.setInsertionPointToStart(dut.getBodyBlock());
  hw::InnerSymbolNamespace dutNS(dut);
  auto wrapperInst =
      b.create<InstanceOp>(b.getUnknownLoc(), wrapper, wrapper.getModuleName(),
                           NameKindEnum::DroppableName, ArrayRef<Attribute>{},
                           ArrayRef<Attribute>{}, false, false,
                           hw::InnerSymAttr::get(b.getStringAttr(
                               dutNS.newName(wrapper.getModuleName()))));
  for (const auto &pair : llvm::enumerate(wrapperInst.getResults())) {
    Value lhs = dut.getArgument(pair.index());
    Value rhs = pair.value();
    if (dut.getPortDirection(pair.index()) == Direction::In)
      std::swap(lhs, rhs);
    emitConnect(b, b.getUnknownLoc(), lhs, rhs);
  }

  // Compute a set of paths that are used _inside_ the wrapper.
  DenseSet<StringAttr> dutPaths, dutPortSyms;
  for (auto anno : AnnotationSet(dut)) {
    auto sym = anno.getMember<FlatSymbolRefAttr>("circt.nonlocal");
    if (sym)
      dutPaths.insert(sym.getAttr());
  }
  for (size_t i = 0, e = dut.getNumPorts(); i != e; ++i) {
    auto portSym = dut.getPortSymbolAttr(i);
    if (portSym)
      dutPortSyms.insert(portSym.getSymName());
    for (auto anno : AnnotationSet::forPort(dut, i)) {
      auto sym = anno.getMember<FlatSymbolRefAttr>("circt.nonlocal");
      if (sym)
        dutPaths.insert(sym.getAttr());
    }
  }

  LLVM_DEBUG({
    llvm::dbgs() << "DUT Symbol Users:\n";
    for (auto path : dutPaths)
      llvm::dbgs() << "  - " << FlatSymbolRefAttr::get(path) << "\n";
    llvm::dbgs() << "Port Symbols:\n";
    for (auto sym : dutPortSyms)
      llvm::dbgs() << "  - " << FlatSymbolRefAttr::get(sym) << "\n";
  });

  // Update NLAs involving the DUT.
  //
  // NOTE: the _DUT_ is the new DUT and all the original DUT contents are put
  // inside the DUT in the _wrapper_.
  //
  // There are three cases to consider:
  //   1. The DUT or a DUT port is a leaf ref.  Do nothing.
  //   2. The DUT is the root.  Update the root module to be the wrapper.
  //   3. The NLA passes through the DUT.  Remove the original InnerRef and
  //      replace it with two InnerRefs: (1) on the DUT and (2) one the wrapper.
  LLVM_DEBUG(llvm::dbgs() << "Processing hierarchical paths:\n");
  auto &nlaTable = getAnalysis<NLATable>();
  DenseMap<StringAttr, hw::HierPathOp> dutRenames;
  for (auto nla : llvm::make_early_inc_range(nlaTable.lookup(dut))) {
    LLVM_DEBUG(llvm::dbgs() << "  - " << nla << "\n");
    auto namepath = nla.getNamepath().getValue();

    // The DUT is the root module.  Just update the root module to point at the
    // wrapper.
    if (nla.root() == dut.getNameAttr()) {
      assert(namepath.size() > 1 && "namepath size must be greater than one");
      SmallVector<Attribute> newNamepath{hw::InnerRefAttr::get(
          wrapper.getNameAttr(),
          cast<hw::InnerRefAttr>(namepath.front()).getName())};
      auto tail = namepath.drop_front();
      newNamepath.append(tail.begin(), tail.end());
      nla->setAttr("namepath", b.getArrayAttr(newNamepath));
      continue;
    }

    // The path ends at the DUT.  This may be a reference path (ends in
    // hw::InnerRefAttr) or a module path (ends in FlatSymbolRefAttr).  There
    // are a number of patterns to disambiguate:
    //
    // NOTE: the _DUT_ is the new DUT and all the original DUT contents are put
    // inside the DUT in the _wrapper_.
    //
    //   1. Reference path on port.  Do nothing.
    //   2. Reference path on component.  Add hierarchy
    //   3. Module path on DUT/DUT port.  Clone path, add hier to original path.
    //   4. Module path on component.  Ad dhierarchy.
    //
    if (nla.leafMod() == dut.getNameAttr()) {
      // Case (1): ref path targeting a port.  Do nothing.
      if (nla.isComponent() && dutPortSyms.count(nla.ref()))
        continue;

      // Case (3): the module path is used by the DUT module or a port. Create a
      // clone of the path and update dutRenames so that this path symbol will
      // get updated for annotations on the DUT or on its ports.
      if (nla.isModule() && dutPaths.contains(nla.getSymNameAttr())) {
        OpBuilder::InsertionGuard guard(b);
        b.setInsertionPoint(nla);
        auto clone = cast<hw::HierPathOp>(b.clone(*nla));
        clone.setSymNameAttr(b.getStringAttr(
            circuitNS.newName(clone.getSymNameAttr().getValue())));
        dutRenames.insert({nla.getSymNameAttr(), clone});
      }

      // Cases (2), (3), and (4): fallthrough to add hierarchy to original path.
    }

    addHierarchy(nla, dut, wrapperInst);
  }

  SmallVector<Annotation> newAnnotations;
  auto removeAndUpdateNLAs = [&](Annotation anno) -> bool {
    auto sym = anno.getMember<FlatSymbolRefAttr>("circt.nonlocal");
    if (!sym)
      return false;
    if (!dutRenames.count(sym.getAttr()))
      return false;
    anno.setMember(
        "circt.nonlocal",
        FlatSymbolRefAttr::get(dutRenames[sym.getAttr()].getSymNameAttr()));
    newAnnotations.push_back(anno);
    return true;
  };

  // Replace any annotations on the DUT or DUT ports to use the cloned path.
  AnnotationSet annotations(dut);
  annotations.removeAnnotations(removeAndUpdateNLAs);
  annotations.addAnnotations(newAnnotations);
  annotations.applyToOperation(dut);
  for (size_t i = 0, e = dut.getNumPorts(); i != e; ++i) {
    newAnnotations.clear();
    auto annotations = AnnotationSet::forPort(dut, i);
    annotations.removeAnnotations(removeAndUpdateNLAs);
    annotations.addAnnotations(newAnnotations);
    annotations.applyToPort(dut, i);
  }

  // Update rwprobe operations' local innerrefs within the module.
  wrapper.walk([&](RWProbeOp rwp) {
    rwp.setTargetAttr(hw::InnerRefAttr::get(wrapper.getModuleNameAttr(),
                                            rwp.getTarget().getName()));
  });
}

//===----------------------------------------------------------------------===//
// Pass Creation
//===----------------------------------------------------------------------===//

std::unique_ptr<mlir::Pass> circt::firrtl::createInjectDUTHierarchyPass() {
  return std::make_unique<InjectDUTHierarchy>();
}
