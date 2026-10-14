import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


EXPECTATIONS = {
    "MissingImmediateMemberSelectsEllipsisFallback": {"result": "long"},
    "ValidImmediateMemberPreservesTemplateCandidate": {"result": "int"},
    "ReferenceToVoidRejectsExplicitCandidate": {"result": "long"},
    "DefaultsSubstituteEarlierArguments": {"valid": "short", "fallback": "long"},
    "TrailingReturnUsesSameImmediateContext": {"result": "long"},
    "ClassInstantiationFailureIsNotSwallowedByFallback": None,
    "EarlierDefaultFailurePreventsLaterClassInstantiation": {"result": "long"},
    "PrefixReturnFailurePreventsParameterClassInstantiation": {"result": "long"},
    "TrailingReturnDoesNotProtectEarlierParameterInstantiation": None,
    "UnusedAliasArgumentStillParticipatesInSubstitution": {"result": "long"},
    "AliasTemplateExpandsWithCanonicalTypeArguments": {"result": "int"},
    "EmptyLocalClassProvesMissingMember": {"result": "long"},
    "ValueMemberIsNotATypeMember": {"result": "long"},
    "ExplicitReferenceTypeDoesNotProvideAClassScope": {"result": "long"},
    "ConflictingDeductionDiscardsOnlyThatCandidate": {"result": "long"},
    "ExplicitArgumentAllowsConversionInsteadOfRededuction": {"result": "long"},
    "DecltypeArithmeticSubstitutesParameterTypes": {"result": "int"},
    "InvalidDecltypeDereferenceSelectsFallback": {"result": "long"},
    "ValidDecltypeDereferencePreservesLvalueCategory": {"result": "int&"},
    "UnparenthesizedAndParenthesizedDecltypeAreDifferent": {
        "plain": "int&&", "reference": "int&"
    },
    "PartialSpecializationBindsItsOwnParameters": {"result": "int"},
    "FailedPartialMatchSelectsPrimaryTemplate": {"result": "long"},
    "PartialSpecializationsUseBidirectionalOrdering": {"result": "int"},
    "AmbiguousPartialSpecializationsRemainAmbiguous": None,
    "VoidAliasPartialSpecializationRejectsMissingType": {"valid": "int", "fallback": "long"},
    "PointerFunctionPatternOutranksUnconstrainedTypePattern": {"result": "long"},
    "FalseAssociatedConstraintPreventsSignatureInstantiation": {"result": "long"},
    "TrueAssociatedConstraintPreservesCandidate": {"result": "int"},
    "TypeRequirementSatisfactionUsesSubstitutedReceiver": {"valid": "int", "fallback": "long"},
    "ConstraintConjunctionStopsBeforeHardInstantiation": {"result": "long"},
    "ConstraintDisjunctionStopsAfterSatisfiedLeftOperand": {"result": "int"},
    "FailedRequirementDoesNotHideRequiredClassInstantiationError": None,
    "ValidNonBooleanAtomicConstraintIsAHardError": None,
    "ConstrainedClassFailureIsImmediateSubstitutionFailure": {"result": "long"},
    "ConceptIdentityParticipatesInConstraintSubsumption": {"result": "int"},
    "NestedAliasWithSpacedClosersSubstitutes": {"result": "int"},
}


def run(compiler, arguments, source=None):
    return subprocess.run(
        [compiler, *arguments], input=source, text=True, capture_output=True,
        timeout=20, check=False
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--spec", type=Path, required=True)
    options = parser.parse_args()
    text = options.spec.read_text(encoding="utf-8")
    fixtures = {}
    sections = re.split(r"TEST\(SfinaeSpec,\s*(\w+)\)", text)
    for index in range(1, len(sections), 2):
        name, body = sections[index:index + 2]
        if name not in EXPECTATIONS:
            continue
        match = re.search(r'R"cpp\((.*?)\)cpp"', body, re.DOTALL)
        if match is None:
            raise AssertionError(f"missing shared fixture: {name}")
        source = match.group(1)
        expected = EXPECTATIONS[name]
        if expected is not None:
            source += "\ntemplate<class, class> constexpr bool Same = false;\n"
            source += "template<class T> constexpr bool Same<T, T> = true;\n"
            for variable, target in expected.items():
                source += f"static_assert(Same<decltype({variable}), {target}>);\n"
        fixtures[name] = source
    assert fixtures.keys() == EXPECTATIONS.keys(), fixtures.keys()

    compilers = [options.compiler]
    clang = shutil.which("clang++")
    if clang and os.path.realpath(clang) != os.path.realpath(options.compiler):
        compilers.append(clang)
    temporary_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    checked = 0
    with tempfile.TemporaryDirectory(prefix="heimdall-sfinae-", dir=temporary_root) as directory:
        directory = Path(directory)
        for compiler in compilers:
            version = run(compiler, ["--version"])
            assert version.returncode == 0, version.stderr
            print(version.stdout.splitlines()[0], flush=True)
            for dialect, alternatives in (
                ("C++20", ["c++20"]),
                ("C++23", ["c++23", "c++2b"]),
                ("C++26", ["c++26", "c++2c"]),
            ):
                standard = None
                for alternative in alternatives:
                    probe = run(compiler, [f"-std={alternative}", "-pedantic-errors",
                                           "-fsyntax-only", "-x", "c++", "-"],
                                "int main() {}\n")
                    if probe.returncode == 0:
                        standard = alternative
                        break
                if standard is None:
                    print(f"SKIP {compiler}: {dialect} unavailable", flush=True)
                    if compiler == options.compiler and dialect == "C++20":
                        raise AssertionError("selected compiler cannot validate the baseline")
                    continue
                for name, source in fixtures.items():
                    path = directory / f"{name}.cpp"
                    path.write_text(source, encoding="utf-8")
                    command = [f"-std={standard}", "-pedantic-errors", "-fsyntax-only", str(path)]
                    result = run(compiler, command)
                    expected_success = EXPECTATIONS[name] is not None
                    assert (result.returncode == 0) == expected_success, (
                        compiler, command, name, expected_success, result.stdout, result.stderr
                    )
                    checked += 1
                print(f"PASS {compiler}: {dialect}, {len(fixtures)} fixtures", flush=True)
    assert checked > 0
    print(f"Validated {checked} compiler/fixture combinations; no binaries executed.")


if __name__ == "__main__":
    main()
