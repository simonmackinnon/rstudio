/*
 * SourceIndex.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <core/libclang/SourceIndex.hpp>

#include <boost/foreach.hpp>

#include <core/FilePath.hpp>
#include <core/PerformanceTimer.hpp>

#include <core/system/ProcessArgs.hpp>

#include <core/libclang/LibClang.hpp>
#include <core/libclang/UnsavedFiles.hpp>

namespace rstudio {
namespace core {
namespace libclang {


bool SourceIndex::isSourceFile(const FilePath& filePath)
{
   std::string ex = filePath.extensionLowerCase();
   return (ex == ".h" || ex == ".hh" || ex == ".hpp" ||
           ex == ".c" || ex == ".cc" || ex == ".cpp" ||
           ex == ".m" || ex == ".mm");
}

bool SourceIndex::isSourceFile(const std::string& filename)
{
   return isSourceFile(FilePath(filename));
}

SourceIndex::SourceIndex(CompilationDatabase compilationDB, int verbose)
{
   verbose_ = verbose;
   index_ = clang().createIndex(0, (verbose_ > 0) ? 1 : 0);
   compilationDB_ = compilationDB;
}

SourceIndex::~SourceIndex()
{
   try
   {
      // remove all
      removeAllTranslationUnits();

      // dispose the index
      if (index_ != NULL)
         clang().disposeIndex(index_);
   }
   catch(...)
   {
   }
}

unsigned SourceIndex::getGlobalOptions() const
{
   return clang().CXIndex_getGlobalOptions(index_);
}

void SourceIndex::setGlobalOptions(unsigned options)
{
   clang().CXIndex_setGlobalOptions(index_, options);
}

void SourceIndex::removeTranslationUnit(const std::string& filename)
{
   TranslationUnits::iterator it = translationUnits_.find(filename);
   if (it != translationUnits_.end())
   {
      if (verbose_ > 0)
         std::cerr << "CLANG REMOVE INDEX: " << it->first << std::endl;
      clang().disposeTranslationUnit(it->second.tu);
      translationUnits_.erase(it->first);
   }
}

void SourceIndex::removeAllTranslationUnits()
{
   for(TranslationUnits::const_iterator it = translationUnits_.begin();
       it != translationUnits_.end(); ++it)
   {
      if (verbose_ > 0)
         std::cerr << "CLANG REMOVE INDEX: " << it->first << std::endl;

      clang().disposeTranslationUnit(it->second.tu);
   }

   translationUnits_.clear();
}


void SourceIndex::primeEditorTranslationUnit(const std::string& filename)
{
   // if we have no record of this translation unit then do a first pass
   if (translationUnits_.find(filename) == translationUnits_.end())
      getTranslationUnit(filename);
}

void SourceIndex::reprimeEditorTranslationUnit(const std::string& filename)
{
   // if we have already indexed this translation unit then re-index it
   if (translationUnits_.find(filename) != translationUnits_.end())
      getTranslationUnit(filename);
}


std::map<std::string,CXTranslationUnit>
                           SourceIndex::getIndexedTranslationUnits() const
{
   std::map<std::string,CXTranslationUnit> units;
   BOOST_FOREACH(const TranslationUnits::value_type& t, translationUnits_)
   {
      units.insert(std::make_pair(t.first, t.second.tu));
   }
   return units;
}

TranslationUnit SourceIndex::getTranslationUnit(const std::string& filename,
                                                bool alwaysReparse)
{
   FilePath filePath(filename);

   boost::scoped_ptr<core::PerformanceTimer> pTimer;
   if (verbose_ > 0)
   {
      std::cerr << "CLANG INDEXING: " << filePath.absolutePath() << std::endl;
      pTimer.reset(new core::PerformanceTimer(filePath.filename()));
   }

   // get the arguments and last write time for this file
   std::vector<std::string> args;
   if (compilationDB_.compileArgsForTranslationUnit)
   {
      args = compilationDB_.compileArgsForTranslationUnit(filename);
      if (args.empty())
         return TranslationUnit();
   }
   std::time_t lastWriteTime = filePath.lastWriteTime();

   // look it up
   TranslationUnits::iterator it = translationUnits_.find(filename);

   // check for various incremental processing scenarios
   if (it != translationUnits_.end())
   {
      // alias record
      StoredTranslationUnit& stored = it->second;

      // already up to date?
      if (!alwaysReparse &&
          (args == stored.compileArgs) &&
          (lastWriteTime == stored.lastWriteTime))
      {
         if (verbose_ > 0)
            std::cerr << "  (Index already up to date)" << std::endl;
         return TranslationUnit(filename, stored.tu, &unsavedFiles_);
      }

      // just needs reparse?
      else if (args == stored.compileArgs)
      {
         if (verbose_ > 0)
         {
            std::string reason = alwaysReparse ?
                                       "(Forced reparse)" :
                                       "(File changed on disk, reparsing)";

            std::cerr << "  " << reason << std::endl;
         }

         int ret = clang().reparseTranslationUnit(
                                stored.tu,
                                unsavedFiles().numUnsavedFiles(),
                                unsavedFiles().unsavedFilesArray(),
                                clang().defaultReparseOptions(stored.tu));

         if (ret == 0)
         {
            // update last write time
            stored.lastWriteTime = lastWriteTime;

            // return it
            return TranslationUnit(filename, stored.tu, &unsavedFiles_);
         }
         else
         {
            LOG_ERROR_MESSAGE("Error re-parsing translation unit " + filename);
         }
      }
   }

   // if we got this far then there either was no existing translation
   // unit or we require a full rebuild. in all cases remove any existing
   // translation unit we have
   removeTranslationUnit(filename);

   // add verbose output if requested
   if (verbose_ >= 2)
     args.push_back("-v");

   // get the args in the fashion libclang expects (char**)
   core::system::ProcessArgs argsArray(args);

   if (verbose_ > 0)
      std::cerr << "  (Creating new index)" << std::endl;

   // create a new translation unit from the file
   CXTranslationUnit tu = clang().parseTranslationUnit(
                         index_,
                         filename.c_str(),
                         argsArray.args(),
                         argsArray.argCount(),
                         unsavedFiles().unsavedFilesArray(),
                         unsavedFiles().numUnsavedFiles(),
                         clang().defaultEditingTranslationUnitOptions());


   // save and return it if we succeeded
   if (tu != NULL)
   {
      translationUnits_[filename] = StoredTranslationUnit(args,
                                                          lastWriteTime,
                                                          tu);

      TranslationUnit unit(filename, tu, &unsavedFiles_);
      if (verbose_ > 0)
         unit.printResourceUsage(std::cerr, false);
      return unit;
   }
   else
   {
      LOG_ERROR_MESSAGE("Error parsing translation unit " + filename);
      return TranslationUnit();
   }
}

} // namespace libclang
} // namespace core
} // namespace rstudio


