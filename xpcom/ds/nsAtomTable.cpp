/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Mutex.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Unused.h"

#include "nsAtom.h"
#include "nsAtomTable.h"
#include "nsStaticAtom.h"
#include "nsString.h"
#include "nsCRT.h"
#include "PLDHashTable.h"
#include "prenv.h"
#include "nsThreadUtils.h"
#include "nsDataHashtable.h"
#include "nsHashKeys.h"
#include "nsAutoPtr.h"
#include "nsUnicharUtils.h"
#include "nsPrintfCString.h"

// There are two kinds of atoms handled by this module.
//
// - Dynamic: the atom itself is heap allocated, as is the nsStringBuffer it
//   points to. |gAtomTable| holds weak references to dynamic atoms. When the
//   refcount of a dynamic atom drops to zero, we increment a static counter.
//   When that counter reaches a certain threshold, we iterate over the atom
//   table, removing and deleting dynamic atoms with refcount zero. This allows
//   us to avoid acquiring the atom table lock during normal refcounting.
//
// - Static: the atom itself is heap allocated, but it points to a static
//   nsStringBuffer. |gAtomTable| effectively owns static atoms, because such
//   atoms ignore all AddRef/Release calls, which ensures they stay alive until
//   |gAtomTable| itself is destroyed whereupon they are explicitly deleted.
//
// Note that gAtomTable is used on multiple threads, and has internal
// synchronization.

using namespace mozilla;

//----------------------------------------------------------------------

enum class GCKind {
  RegularOperation,
  Shutdown,
};

//----------------------------------------------------------------------

// gUnusedAtomCount is incremented when an atom loses its last reference
// (and thus turned into unused state), and decremented when an unused
// atom gets a reference again. The atom table relies on this value to
// schedule GC. This value can temporarily go below zero when multiple
// threads are operating the same atom, so it has to be signed so that
// we wouldn't use overflow value for comparison.
// See nsAtom::AddRef() and nsAtom::Release().
static Atomic<int32_t, ReleaseAcquire> gUnusedAtomCount(0);

// Dynamic atoms need a ref count; this class adds that to nsAtom.
class nsDynamicAtom : public nsAtom
{
public:
  // We can't use NS_INLINE_DECL_THREADSAFE_REFCOUNTING because the refcounting
  // of this type is special.
  MozExternalRefCountType AddRef();
  MozExternalRefCountType Release();

  static nsDynamicAtom* As(nsAtom* aAtom)
  {
    MOZ_ASSERT(aAtom->IsDynamicAtom());
    return static_cast<nsDynamicAtom*>(aAtom);
  }

private:
  friend class nsAtomTable;
  friend class nsAtomSubTable;

  // Construction is done by |friend|s.
  nsDynamicAtom(const nsAString& aString, uint32_t aHash)
    : nsAtom(AtomKind::DynamicAtom, aString, aHash)
    , mRefCnt(1)
  {}

  mozilla::ThreadSafeAutoRefCnt mRefCnt;
};

static char16_t*
FromStringBuffer(const nsAString& aString)
{
  char16_t* str;
  size_t length = aString.Length();
  RefPtr<nsStringBuffer> buf = nsStringBuffer::FromString(aString);
  if (buf) {
    str = static_cast<char16_t*>(buf->Data());
  } else {
    const size_t size = (length + 1) * sizeof(char16_t);
    buf = nsStringBuffer::Alloc(size);
    if (MOZ_UNLIKELY(!buf)) {
      NS_ABORT_OOM(size); // OOM because atom allocations should be small.
    }
    str = static_cast<char16_t*>(buf->Data());
    CopyUnicodeTo(aString, 0, str, length);
    str[length] = char16_t(0);
  }

  MOZ_ASSERT(buf && buf->StorageSize() >= (length + 1) * sizeof(char16_t),
             "enough storage");

  // Take ownership of the string buffer.
  mozilla::Unused << buf.forget();

  return str;
}

// This constructor is for dynamic atoms and HTML5 atoms.
nsAtom::nsAtom(AtomKind aKind, const nsAString& aString, uint32_t aHash)
  : mLength(aString.Length())
  , mKind(static_cast<uint32_t>(aKind))
  , mHash(aHash)
  , mString(FromStringBuffer(aString))
{
  MOZ_ASSERT(aKind == AtomKind::DynamicAtom || aKind == AtomKind::HTML5Atom);

  MOZ_ASSERT_IF(!IsHTML5Atom(), mHash == HashString(mString, mLength));

  MOZ_ASSERT(mString[mLength] == char16_t(0), "null terminated");
  MOZ_ASSERT(Equals(aString), "correct data");
}

// This constructor is for static atoms.
nsAtom::nsAtom(const char16_t* aString, uint32_t aLength, uint32_t aHash)
  : mLength(aLength)
  , mKind(static_cast<uint32_t>(AtomKind::StaticAtom))
  , mHash(aHash)
  , mString(const_cast<char16_t*>(aString))
{
  MOZ_ASSERT(mHash == HashString(mString, mLength));

  MOZ_ASSERT(mString[mLength] == char16_t(0), "null terminated");
  MOZ_ASSERT(NS_strlen(mString) == mLength, "correct storage");
}

nsAtom::~nsAtom()
{
  if (!IsStaticAtom()) {
    MOZ_ASSERT(IsDynamicAtom() || IsHTML5Atom());
    GetStringBuffer()->Release();
  }
}

void
nsAtom::ToString(nsAString& aString) const
{
  // See the comment on |mString|'s declaration.
  if (IsStaticAtom()) {
    // AssignLiteral() lets us assign without copying. This isn't a string
    // literal, but it's a static atom and thus has an unbounded lifetime,
    // which is what's important.
    aString.AssignLiteral(mString, mLength);
  } else {
    GetStringBuffer()->ToString(mLength, aString);
  }
}

void
nsAtom::ToUTF8String(nsACString& aBuf) const
{
  MOZ_ASSERT(!IsHTML5Atom(), "Called ToUTF8String() on an HTML5 atom");
  CopyUTF16toUTF8(nsDependentString(mString, mLength), aBuf);
}

void
nsAtom::AddSizeOfIncludingThis(MallocSizeOf aMallocSizeOf, AtomsSizes& aSizes)
  const
{
  MOZ_ASSERT(!IsHTML5Atom(),
             "Called AddSizeOfIncludingThis() on an HTML5 atom");
  size_t thisSize = aMallocSizeOf(this);
  if (IsStaticAtom()) {
    // String buffers pointed to by static atoms are in static memory, and so
    // are not measured here.
    aSizes.mStaticAtomObjects += thisSize;
  } else {
    aSizes.mDynamicAtomObjects += thisSize;
    aSizes.mDynamicUnsharedBuffers +=
      GetStringBuffer()->SizeOfIncludingThisIfUnshared(aMallocSizeOf);
  }
}

//----------------------------------------------------------------------

struct AtomTableKey
{
  AtomTableKey(const char16_t* aUTF16String, uint32_t aLength,
               uint32_t* aHashOut)
    : mUTF16String(aUTF16String)
    , mUTF8String(nullptr)
    , mLength(aLength)
  {
    mHash = HashString(mUTF16String, mLength);
    *aHashOut = mHash;
  }

  AtomTableKey(const char* aUTF8String, uint32_t aLength, uint32_t* aHashOut)
    : mUTF16String(nullptr)
    , mUTF8String(aUTF8String)
    , mLength(aLength)
  {
    bool err;
    mHash = HashUTF8AsUTF16(mUTF8String, mLength, &err);
    if (err) {
      mUTF8String = nullptr;
      mLength = 0;
      mHash = 0;
    }
    *aHashOut = mHash;
  }

  const char16_t* mUTF16String;
  const char* mUTF8String;
  uint32_t mLength;
  uint32_t mHash;
};

struct AtomTableEntry : public PLDHashEntryHdr
{
  // These references are either to dynamic atoms, in which case they are
  // non-owning, or they are to static atoms, which aren't really refcounted.
  // See the comment at the top of this file for more details.
  nsAtom* MOZ_NON_OWNING_REF mAtom;
};

#define RECENTLY_USED_MAIN_THREAD_ATOM_CACHE_SIZE 31
static nsAtom*
  sRecentlyUsedMainThreadAtoms[RECENTLY_USED_MAIN_THREAD_ATOM_CACHE_SIZE] = {};

// In order to reduce locking contention for concurrent atomization, we segment
// the atom table into N subtables, each with a separate lock. If the hash
// values we use to select the subtable are evenly distributed, this reduces the
// probability of contention by a factor of N. See bug 1440824.
//
// NB: This is somewhat similar to the technique used by Java's
// ConcurrentHashTable.
class nsAtomSubTable
{
  friend class nsAtomTable;
  Mutex mLock;
  PLDHashTable mTable;
  nsAtomSubTable();
  void GCLocked(GCKind aKind);
  void AddSizeOfExcludingThisLocked(MallocSizeOf aMallocSizeOf,
                                    AtomsSizes& aSizes);

  AtomTableEntry* Search(AtomTableKey& aKey)
  {
    mLock.AssertCurrentThreadOwns();
    return static_cast<AtomTableEntry*>(mTable.Search(&aKey));
  }

  AtomTableEntry* Add(AtomTableKey& aKey)
  {
    mLock.AssertCurrentThreadOwns();
    return static_cast<AtomTableEntry*>(mTable.Add(&aKey)); // Infallible
  }
};

// The outer atom table, which coordinates access to the inner array of
// subtables.
class nsAtomTable
{
public:
  nsAtomSubTable& SelectSubTable(AtomTableKey& aKey);
  void AddSizeOfIncludingThis(MallocSizeOf aMallocSizeOf, AtomsSizes& aSizes);
  void GC(GCKind aKind);
  already_AddRefed<nsAtom> Atomize(const nsAString& aUTF16String);
  already_AddRefed<nsAtom> Atomize(const nsACString& aUTF8String);
  already_AddRefed<nsAtom> AtomizeMainThread(const nsAString& aUTF16String);
  nsStaticAtom* GetStaticAtom(const nsAString& aUTF16String);
  void RegisterStaticAtoms(const nsStaticAtomSetup* aSetup, uint32_t aCount);

  // The result of this function may be imprecise if other threads are operating
  // on atoms concurrently. It's also slow, since it triggers a GC before
  // counting.
  size_t RacySlowCount();

  // This hash table op is a static member of this class so that it can take
  // advantage of |friend| declarations.
  static void AtomTableClearEntry(PLDHashTable* aTable,
                                  PLDHashEntryHdr* aEntry);

  // We achieve measurable reduction in locking contention in parallel CSS
  // parsing by increasing the number of subtables up to 128. This has been
  // measured to have neglible impact on the performance of initialization, GC,
  // and shutdown.
  //
  // Another important consideration is memory, since we're adding fixed
  // overhead per content process, which we try to avoid. Measuring a
  // mostly-empty page [1] with various numbers of subtables, we get the
  // following deep sizes for the atom table:
  //       1 subtable:  278K
  //       8 subtables: 279K
  //      16 subtables: 282K
  //      64 subtables: 286K
  //     128 subtables: 290K
  //
  // So 128 subtables costs us 12K relative to a single table, and 4K relative
  // to 64 subtables. Conversely, measuring parallel (6 thread) CSS parsing on
  // tp6-facebook, a single table provides ~150ms of locking overhead per
  // thread, 64 subtables provides ~2-3ms of overhead, and 128 subtables
  // provides <1ms. And so while either 64 or 128 subtables would probably be
  // acceptable, achieving a measurable reduction in contention for 4k of fixed
  // memory overhead is probably worth it.
  //
  // [1] The numbers will look different for content processes with complex
  // pages loaded, but in those cases the actual atoms will dominate memory
  // usage and the overhead of extra tables will be negligible. We're mostly
  // interested in the fixed cost for nearly-empty content processes.
  const static size_t kNumSubTables = 128; // Must be power of two.

private:
  nsAtomSubTable mSubTables[kNumSubTables];
};

// Static singleton instance for the atom table.
static nsAtomTable* gAtomTable;

static PLDHashNumber
AtomTableGetHash(const void* aKey)
{
  const AtomTableKey* k = static_cast<const AtomTableKey*>(aKey);
  return k->mHash;
}

static bool
AtomTableMatchKey(const PLDHashEntryHdr* aEntry, const void* aKey)
{
  const AtomTableEntry* he = static_cast<const AtomTableEntry*>(aEntry);
  const AtomTableKey* k = static_cast<const AtomTableKey*>(aKey);

  if (k->mUTF8String) {
    return
      CompareUTF8toUTF16(nsDependentCSubstring(k->mUTF8String,
                                               k->mUTF8String + k->mLength),
                         nsDependentAtomString(he->mAtom)) == 0;
  }

  return he->mAtom->Equals(k->mUTF16String, k->mLength);
}

void
nsAtomTable::AtomTableClearEntry(PLDHashTable* aTable, PLDHashEntryHdr* aEntry)
{
  auto entry = static_cast<AtomTableEntry*>(aEntry);
  nsAtom* atom = entry->mAtom;
  if (atom->IsStaticAtom()) {
    // This case -- when the entry being cleared holds a static atom -- only
    // occurs when gAtomTable is destroyed, whereupon all static atoms within it
    // must be explicitly deleted.
    delete atom;
  }
}

static void
AtomTableInitEntry(PLDHashEntryHdr* aEntry, const void* aKey)
{
  static_cast<AtomTableEntry*>(aEntry)->mAtom = nullptr;
}

static const PLDHashTableOps AtomTableOps = {
  AtomTableGetHash,
  AtomTableMatchKey,
  PLDHashTable::MoveEntryStub,
  nsAtomTable::AtomTableClearEntry,
  AtomTableInitEntry
};

// The atom table very quickly gets 10,000+ entries in it (or even 100,000+).
// But choosing the best initial subtable length has some subtleties: we add
// ~2700 static atoms at start-up, and then we start adding and removing
// dynamic atoms. If we make the tables too big to start with, when the first
// dynamic atom gets removed from a given table the load factor will be < 25%
// and we will shrink it.
//
// So we first make the simplifying assumption that the atoms are more or less
// evenly-distributed across the subtables (which is the case empirically).
// Then, we take the total atom count when the first dynamic atom is removed
// (~2700), divide that across the N subtables, and the largest capacity that
// will allow each subtable to be > 25% full with that count.
//
// So want an initial subtable capacity less than (2700 / N) * 4 = 10800 / N.
// Rounding down to the nearest power of two gives us 8192 / N. Since the
// capacity is double the initial length, we end up with (4096 / N) per subtable.
#define INITIAL_SUBTABLE_LENGTH (4096 / nsAtomTable::kNumSubTables)

nsAtomSubTable&
nsAtomTable::SelectSubTable(AtomTableKey& aKey)
{
  // There are a few considerations around how we select subtables.
  //
  // First, we want entries to be evenly distributed across the subtables. This
  // can be achieved by using any bits in the hash key, assuming the key itself
  // is evenly-distributed. Empirical measurements indicate that this method
  // produces a roughly-even distribution across subtables.
  //
  // Second, we want to use the hash bits that are least likely to influence an
  // entry's position within the subtable. If we used the exact same bits used
  // by the subtables, then each subtable would compute the same position for
  // every entry it observes, leading to pessimal performance. In this case,
  // we're using PLDHashTable, whose primary hash function uses the N leftmost
  // bits of the hash value (where N is the log2 capacity of the table). This
  // means we should prefer the rightmost bits here.
  //
  // Note that the below is equivalent to mHash % kNumSubTables, a replacement
  // which an optimizing compiler should make, but let's avoid any doubt.
  static_assert((kNumSubTables & (kNumSubTables - 1)) == 0, "must be power of two");
  return mSubTables[aKey.mHash & (kNumSubTables - 1)];
}

void
nsAtomTable::AddSizeOfIncludingThis(MallocSizeOf aMallocSizeOf,
                                    AtomsSizes& aSizes)
{
  MOZ_ASSERT(NS_IsMainThread());
  aSizes.mTable += aMallocSizeOf(this);
  for (auto& table : mSubTables) {
    MutexAutoLock lock(table.mLock);
    table.AddSizeOfExcludingThisLocked(aMallocSizeOf, aSizes);
  }
}

void nsAtomTable::GC(GCKind aKind)
{
  MOZ_ASSERT(NS_IsMainThread());
  for (uint32_t i = 0; i < RECENTLY_USED_MAIN_THREAD_ATOM_CACHE_SIZE; ++i) {
    sRecentlyUsedMainThreadAtoms[i] = nullptr;
  }

  // Note that this is effectively an incremental GC, since only one subtable
  // is locked at a time.
  for (auto& table: mSubTables) {
    MutexAutoLock lock(table.mLock);
    table.GCLocked(aKind);
  }

  // We would like to assert that gUnusedAtomCount matches the number of atoms
  // we found in the table which we removed. However, there are two problems
  // with this:
  // * We have multiple subtables, each with their own lock. For optimal
  //   performance we only want to hold one lock at a time, but this means
  //   that atoms can be added and removed between GC slices.
  // * Even if we held all the locks and performed all GC slices atomically,
  //   the locks are not acquired for AddRef() and Release() calls. This means
  //   we might see a gUnusedAtomCount value in between, say, AddRef()
  //   incrementing mRefCnt and it decrementing gUnusedAtomCount.
  //
  // So, we don't bother asserting that there are no unused atoms at the end of
  // a regular GC. But we can (and do) assert this just after the last GC at
  // shutdown.
  //
  // Note that, barring refcounting bugs, an atom can only go from a zero
  // refcount to a non-zero refcount while the atom table lock is held, so
  // so we won't try to resurrect a zero refcount atom while trying to delete
  // it.

  MOZ_ASSERT_IF(aKind == GCKind::Shutdown, gUnusedAtomCount == 0);
}

size_t
nsAtomTable::RacySlowCount()
{
  // Trigger a GC so that the result is deterministic modulo other threads.
  GC(GCKind::RegularOperation);
  size_t count = 0;
  for (auto& table: mSubTables) {
    MutexAutoLock lock(table.mLock);
    count += table.mTable.EntryCount();
  }

  return count;
}

nsAtomSubTable::nsAtomSubTable()
  : mLock("Atom Sub-Table Lock")
  , mTable(&AtomTableOps, sizeof(AtomTableEntry), INITIAL_SUBTABLE_LENGTH)
{
}

void
nsAtomSubTable::GCLocked(GCKind aKind)
{
  MOZ_ASSERT(NS_IsMainThread());
  mLock.AssertCurrentThreadOwns();

  int32_t removedCount = 0; // A non-atomic temporary for cheaper increments.
  nsAutoCString nonZeroRefcountAtoms;
  uint32_t nonZeroRefcountAtomsCount = 0;
  for (auto i = mTable.Iter(); !i.Done(); i.Next()) {
    auto entry = static_cast<AtomTableEntry*>(i.Get());
    if (entry->mAtom->IsStaticAtom()) {
      continue;
    }

    nsAtom* atom = entry->mAtom;
    MOZ_ASSERT(!atom->IsHTML5Atom());
    if (atom->IsDynamicAtom() && nsDynamicAtom::As(atom)->mRefCnt == 0) {
      i.Remove();
      delete atom;
      ++removedCount;
    }
#ifdef NS_FREE_PERMANENT_DATA
    else if (aKind == GCKind::Shutdown && PR_GetEnv("XPCOM_MEM_BLOAT_LOG")) {
      // Only report leaking atoms in leak-checking builds in a run where we
      // are checking for leaks, during shutdown. If something is anomalous,
      // then we'll assert later in this function.
      nsAutoCString name;
      atom->ToUTF8String(name);
      if (nonZeroRefcountAtomsCount == 0) {
        nonZeroRefcountAtoms = name;
      } else if (nonZeroRefcountAtomsCount < 20) {
        nonZeroRefcountAtoms += NS_LITERAL_CSTRING(",") + name;
      } else if (nonZeroRefcountAtomsCount == 20) {
        nonZeroRefcountAtoms += NS_LITERAL_CSTRING(",...");
      }
      nonZeroRefcountAtomsCount++;
    }
#endif

  }
  if (nonZeroRefcountAtomsCount) {
    nsPrintfCString msg("%d dynamic atom(s) with non-zero refcount: %s",
                        nonZeroRefcountAtomsCount, nonZeroRefcountAtoms.get());
    NS_ASSERTION(nonZeroRefcountAtomsCount == 0, msg.get());
  }

  gUnusedAtomCount -= removedCount;
}

static void
GCAtomTable()
{
  MOZ_ASSERT(gAtomTable);
  if (NS_IsMainThread()) {
    gAtomTable->GC(GCKind::RegularOperation);
  }
}

MOZ_ALWAYS_INLINE MozExternalRefCountType
nsDynamicAtom::AddRef()
{
  MOZ_ASSERT(int32_t(mRefCnt) >= 0, "illegal refcnt");
  nsrefcnt count = ++mRefCnt;
  if (count == 1) {
    gUnusedAtomCount--;
  }
  return count;
}

MOZ_ALWAYS_INLINE MozExternalRefCountType
nsDynamicAtom::Release()
{
  #ifdef DEBUG
  // We set a lower GC threshold for atoms in debug builds so that we exercise
  // the GC machinery more often.
  static const int32_t kAtomGCThreshold = 20;
  #else
  static const int32_t kAtomGCThreshold = 10000;
  #endif

  MOZ_ASSERT(int32_t(mRefCnt) > 0, "dup release");
  nsrefcnt count = --mRefCnt;
  if (count == 0) {
    if (++gUnusedAtomCount >= kAtomGCThreshold) {
      GCAtomTable();
    }
  }

  return count;
}

MozExternalRefCountType
nsAtom::AddRef()
{
  MOZ_ASSERT(!IsHTML5Atom(), "Attempt to AddRef an HTML5 atom");

  return IsStaticAtom() ? 2 : nsDynamicAtom::As(this)->AddRef();
}

MozExternalRefCountType
nsAtom::Release()
{
  MOZ_ASSERT(!IsHTML5Atom(), "Attempt to Release an HTML5 atom");

  return IsStaticAtom() ? 1 : nsDynamicAtom::As(this)->Release();
}

//----------------------------------------------------------------------

// Have the static atoms been inserted into the table?
static bool gStaticAtomsDone = false;

class DefaultAtoms
{
public:
  NS_STATIC_ATOM_DECL(empty)
};

NS_STATIC_ATOM_DEFN(DefaultAtoms, empty)

NS_STATIC_ATOM_BUFFER(empty, "")

static const nsStaticAtomSetup sDefaultAtomSetup[] = {
  NS_STATIC_ATOM_SETUP(DefaultAtoms, empty)
};

void
NS_InitAtomTable()
{
  MOZ_ASSERT(!gAtomTable);
  gAtomTable = new nsAtomTable();

  // Bug 1340710 has caused us to generate an empty atom at arbitrary times
  // after startup.  If we end up creating one before nsGkAtoms::_empty is
  // registered, we get an assertion about transmuting a dynamic atom into a
  // static atom.  In order to avoid that, we register an empty string static
  // atom as soon as we initialize the atom table to guarantee that the empty
  // string atom will always be static.
  NS_RegisterStaticAtoms(sDefaultAtomSetup);
}

void
NS_ShutdownAtomTable()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(gAtomTable);

#ifdef NS_FREE_PERMANENT_DATA
  // Do a final GC to satisfy leak checking. We skip this step in release
  // builds.
  gAtomTable->GC(GCKind::Shutdown);
#endif

  delete gAtomTable;
  gAtomTable = nullptr;
}

void
NS_AddSizeOfAtoms(MallocSizeOf aMallocSizeOf, AtomsSizes& aSizes)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->AddSizeOfIncludingThis(aMallocSizeOf, aSizes);
}

void
nsAtomSubTable::AddSizeOfExcludingThisLocked(MallocSizeOf aMallocSizeOf,
                                             AtomsSizes& aSizes)
{
  mLock.AssertCurrentThreadOwns();
  aSizes.mTable += mTable.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (auto iter = mTable.Iter(); !iter.Done(); iter.Next()) {
    auto entry = static_cast<AtomTableEntry*>(iter.Get());
    entry->mAtom->AddSizeOfIncludingThis(aMallocSizeOf, aSizes);
  }
}

void
nsAtomTable::RegisterStaticAtoms(const nsStaticAtomSetup* aSetup,
                                 uint32_t aCount)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_RELEASE_ASSERT(!gStaticAtomsDone, "Static atom insertion is finished!");

  for (uint32_t i = 0; i < aCount; ++i) {
    const char16_t* string = aSetup[i].mString;
    nsStaticAtom** atomp = aSetup[i].mAtom;

    MOZ_ASSERT(nsCRT::IsAscii(string));

    uint32_t stringLen = NS_strlen(string);

    uint32_t hash;
    AtomTableKey key(string, stringLen, &hash);
    nsAtomSubTable& table = SelectSubTable(key);
    MutexAutoLock lock(table.mLock);
    AtomTableEntry* he = table.Add(key);

    nsStaticAtom* atom;
    if (he->mAtom) {
      // Disallow creating a dynamic atom, and then later, while the dynamic
      // atom is still alive, registering that same atom as a static atom.  It
      // causes subtle bugs, and we're programming in C++ here, not Smalltalk.
      if (!he->mAtom->IsStaticAtom()) {
        nsAutoCString name;
        he->mAtom->ToUTF8String(name);
        MOZ_CRASH_UNSAFE_PRINTF(
          "Static atom registration for %s should be pushed back", name.get());
      }
      atom = static_cast<nsStaticAtom*>(he->mAtom);
    } else {
      atom = new nsStaticAtom(string, stringLen, hash);
      he->mAtom = atom;
    }
    *atomp = atom;
  }
}

void
RegisterStaticAtoms(const nsStaticAtomSetup* aSetup, uint32_t aCount)
{
  MOZ_ASSERT(gAtomTable);
  gAtomTable->RegisterStaticAtoms(aSetup, aCount);
}

already_AddRefed<nsAtom>
NS_Atomize(const char* aUTF8String)
{
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->Atomize(nsDependentCString(aUTF8String));
}

already_AddRefed<nsAtom>
nsAtomTable::Atomize(const nsACString& aUTF8String)
{
  uint32_t hash;
  AtomTableKey key(aUTF8String.Data(), aUTF8String.Length(), &hash);
  nsAtomSubTable& table = SelectSubTable(key);
  MutexAutoLock lock(table.mLock);
  AtomTableEntry* he = table.Add(key);

  if (he->mAtom) {
    RefPtr<nsAtom> atom = he->mAtom;

    return atom.forget();
  }

  // This results in an extra addref/release of the nsStringBuffer.
  // Unfortunately there doesn't seem to be any APIs to avoid that.
  // Actually, now there is, sort of: ForgetSharedBuffer.
  nsString str;
  CopyUTF8toUTF16(aUTF8String, str);
  RefPtr<nsAtom> atom = dont_AddRef(new nsDynamicAtom(str, hash));

  he->mAtom = atom;

  return atom.forget();
}

already_AddRefed<nsAtom>
NS_Atomize(const nsACString& aUTF8String)
{
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->Atomize(aUTF8String);
}

already_AddRefed<nsAtom>
NS_Atomize(const char16_t* aUTF16String)
{
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->Atomize(nsDependentString(aUTF16String));
}

already_AddRefed<nsAtom>
nsAtomTable::Atomize(const nsAString& aUTF16String)
{
  uint32_t hash;
  AtomTableKey key(aUTF16String.Data(), aUTF16String.Length(), &hash);
  nsAtomSubTable& table = SelectSubTable(key);
  MutexAutoLock lock(table.mLock);
  AtomTableEntry* he = table.Add(key);

  if (he->mAtom) {
    RefPtr<nsAtom> atom = he->mAtom;

    return atom.forget();
  }

  RefPtr<nsAtom> atom = dont_AddRef(new nsDynamicAtom(aUTF16String, hash));
  he->mAtom = atom;

  return atom.forget();
}

already_AddRefed<nsAtom>
NS_Atomize(const nsAString& aUTF16String)
{
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->Atomize(aUTF16String);
}

already_AddRefed<nsAtom>
nsAtomTable::AtomizeMainThread(const nsAString& aUTF16String)
{
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<nsAtom> retVal;
  uint32_t hash;
  AtomTableKey key(aUTF16String.Data(), aUTF16String.Length(), &hash);
  uint32_t index = hash % RECENTLY_USED_MAIN_THREAD_ATOM_CACHE_SIZE;
  nsAtom* atom = sRecentlyUsedMainThreadAtoms[index];
  if (atom) {
    uint32_t length = atom->GetLength();
    if (length == key.mLength &&
        (memcmp(atom->GetUTF16String(),
                key.mUTF16String, length * sizeof(char16_t)) == 0)) {
      retVal = atom;
      return retVal.forget();
    }
  }

  nsAtomSubTable& table = SelectSubTable(key);
  MutexAutoLock lock(table.mLock);
  AtomTableEntry* he = table.Add(key);

  if (he->mAtom) {
    retVal = he->mAtom;
  } else {
    RefPtr<nsAtom> newAtom = dont_AddRef(new nsDynamicAtom(aUTF16String, hash));
    he->mAtom = newAtom;
    retVal = newAtom.forget();
  }

  sRecentlyUsedMainThreadAtoms[index] = he->mAtom;
  return retVal.forget();
}

already_AddRefed<nsAtom>
NS_AtomizeMainThread(const nsAString& aUTF16String)
{
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->AtomizeMainThread(aUTF16String);
}

nsrefcnt
NS_GetNumberOfAtoms(void)
{
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->RacySlowCount();
}

int32_t
NS_GetUnusedAtomCount(void)
{
  return gUnusedAtomCount;
}

nsStaticAtom*
NS_GetStaticAtom(const nsAString& aUTF16String)
{
  MOZ_ASSERT(gStaticAtomsDone, "Static atom setup not yet done.");
  MOZ_ASSERT(gAtomTable);
  return gAtomTable->GetStaticAtom(aUTF16String);
}

nsStaticAtom*
nsAtomTable::GetStaticAtom(const nsAString& aUTF16String)
{
  uint32_t hash;
  AtomTableKey key(aUTF16String.Data(), aUTF16String.Length(), &hash);
  nsAtomSubTable& table = SelectSubTable(key);
  MutexAutoLock lock(table.mLock);
  AtomTableEntry* he = table.Search(key);
  return he && he->mAtom->IsStaticAtom()
       ? static_cast<nsStaticAtom*>(he->mAtom)
       : nullptr;
}

void
NS_SetStaticAtomsDone()
{
  MOZ_ASSERT(NS_IsMainThread());
  gStaticAtomsDone = true;
}
