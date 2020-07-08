//===- ExceptionDecoder.cpp -------------------------------------*- C++ -*-===//
//
//  Copyright (C) 2019 GrammaTech, Inc.
//
//  This code is licensed under the GNU Affero General Public License
//  as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version. See the
//  LICENSE.txt file in the project root for license terms or visit
//  https://www.gnu.org/licenses/agpl.txt.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//  GNU Affero General Public License for more details.
//
//  This project is sponsored by the Office of Naval Research, One Liberty
//  Center, 875 N. Randolph Street, Arlington, VA 22203 under contract #
//  N68335-17-C-0700.  The content of the information does not necessarily
//  reflect the position or policy of the Government and no official
//  endorsement should be inferred.
//
//===----------------------------------------------------------------------===//

#include "ExceptionDecoder.h"

ExceptionDecoder::ExceptionDecoder(gtirb::Module &module)
{
    uint8_t ptrsize(8);
    std::string ehFrame, ehFrameHeader, gccExcept;
    uint64_t addressEhFrame(0), addressEhFrameHeader(0), addressGccExcept(0);

    auto ehFrameSection = module.findSections(".eh_frame");
    if(ehFrameSection != module.sections_by_name_end())
    {
        assert(ehFrameSection->getAddress() && "Found .eh_frame section without an address.");
        addressEhFrame = static_cast<uint64_t>(*ehFrameSection->getAddress());
        if(auto it = ehFrameSection->findByteIntervalsAt(*ehFrameSection->getAddress());
           !it.empty())
        {
            const gtirb::ByteInterval &interval = *it.begin();
            assert(ehFrameSection->getSize() == interval.getSize()
                   && "Expected single .eh_frame byte interval.");

            const char *bytes = interval.rawBytes<const char>();
            const char *end = bytes + interval.getInitializedSize();
            ehFrame.assign(bytes, end);
        }
    }
    auto ehFrameHeaderSection = module.findSections(".eh_frame_hdr");
    if(ehFrameHeaderSection != module.sections_by_name_end())
    {
        assert(ehFrameHeaderSection->getAddress()
               && "Found .eh_frame_hdr section without an address.");
        addressEhFrameHeader = static_cast<uint64_t>(*ehFrameHeaderSection->getAddress());
        if(auto it = ehFrameHeaderSection->findByteIntervalsAt(*ehFrameHeaderSection->getAddress());
           !it.empty())
        {
            const gtirb::ByteInterval &interval = *it.begin();
            assert(ehFrameHeaderSection->getSize() == interval.getSize()
                   && "Expected single .eh_frame_hdr byte interval.");

            const char *bytes = interval.rawBytes<const char>();
            const char *end = bytes + interval.getInitializedSize();
            ehFrameHeader.assign(bytes, end);
        }
    }
    auto gccExceptSection = module.findSections(".gcc_except_table");
    if(gccExceptSection != module.sections_by_name_end())
    {
        assert(gccExceptSection->getAddress()
               && "Found .gcc_except_table section without an address.");
        addressGccExcept = static_cast<uint64_t>(*gccExceptSection->getAddress());
        if(auto it = gccExceptSection->findByteIntervalsAt(*gccExceptSection->getAddress());
           !it.empty())
        {
            const gtirb::ByteInterval &interval = *it.begin();
            assert(gccExceptSection->getSize() == interval.getSize()
                   && "Expected single .gcc_except_table byte interval.");

            const char *bytes = interval.rawBytes<char>();
            const char *end = bytes + interval.getInitializedSize();
            gccExcept.assign(bytes, end);
        }
    }
    ehParser = EHP::EHFrameParser_t::factory(ptrsize, EHP::EHPEndianness_t::HOST, ehFrame,
                                             addressEhFrame, ehFrameHeader, addressEhFrameHeader,
                                             gccExcept, addressGccExcept);
}

void ExceptionDecoder::addExceptionInformation(souffle::SouffleProgram *prog)
{
    auto *cieRelation = prog->getRelation("cie_entry");
    auto *cieEncodingRelation = prog->getRelation("cie_encoding");
    auto *ciePersonalityRelation = prog->getRelation("cie_personality");
    for(const EHP::CIEContents_t *cie : *(ehParser->getCIEs()))
    {
        cieRelation->insert(getCIEEntry(cieRelation, cie));
        cieEncodingRelation->insert(getCIEEncoding(cieEncodingRelation, cie));
        ciePersonalityRelation->insert(getCIEPersonality(ciePersonalityRelation, cie));
    }

    auto *fdeRelation = prog->getRelation("fde_entry");
    auto *fdePtrLocationsRelation = prog->getRelation("fde_pointer_locations");
    auto *fdeInsnRelation = prog->getRelation("fde_instruction");
    auto *lsdaRelation = prog->getRelation("lsda");
    auto *lsdaPtrLocationsRelation = prog->getRelation("lsda_pointer_locations");
    auto *callSiteRelation = prog->getRelation("lsda_callsite");
    auto *typeEntryRelation = prog->getRelation("lsda_type_entry");

    for(const EHP::FDEContents_t *fde : *(ehParser->getFDEs()))
    {
        fdeRelation->insert(getFDE(fdeRelation, fde));
        fdePtrLocationsRelation->insert(getFDEPointerLocations(fdePtrLocationsRelation, fde));
        uint64_t insnIndex = 0;
        for(const EHP::EHProgramInstruction_t *insn :
            *(fde->getCIE().getProgram().getInstructions()))
        {
            fdeInsnRelation->insert(getEHProgramInstruction(fdeInsnRelation, insnIndex, insn, fde));
            ++insnIndex;
        }
        for(const EHP::EHProgramInstruction_t *insn : *(fde->getProgram().getInstructions()))
        {
            fdeInsnRelation->insert(getEHProgramInstruction(fdeInsnRelation, insnIndex, insn, fde));
            ++insnIndex;
        }

        auto *lsda = fde->getLSDA();
        if(lsda && fde->getLSDAAddress() != 0)
        {
            lsdaRelation->insert(getLSDA(lsdaRelation, lsda, fde));
            lsdaPtrLocationsRelation->insert(
                getLSDAPointerLocations(lsdaPtrLocationsRelation, lsda, fde));
            for(const EHP::LSDACallSite_t *callSite : *(lsda->getCallSites()))
            {
                callSiteRelation->insert(getLSDACallSite(callSiteRelation, callSite, lsda));
            }
            uint64_t index = 0;
            for(const EHP::LSDATypeTableEntry_t *typeEntry : *(lsda->getTypeTable()))
            {
                typeEntryRelation->insert(
                    getLSDATypetableEntry(typeEntryRelation, index, typeEntry, lsda));
                ++index;
            }
        }
    }
}

souffle::tuple ExceptionDecoder::getCIEEntry(souffle::Relation *relation,
                                             const EHP::CIEContents_t *cie)
{
    souffle::tuple tuple(relation);
    tuple << cie->getPosition() << cie->getLength() << cie->getCAF() << cie->getDAF();
    return tuple;
}

souffle::tuple ExceptionDecoder::getCIEEncoding(souffle::Relation *relation,
                                                const EHP::CIEContents_t *cie)
{
    souffle::tuple tuple(relation);
    uint64_t fdeEnconding = cie->getFDEEncoding();
    uint64_t lsdaEncoding = cie->getLSDAEncoding();
    tuple << cie->getPosition() << fdeEnconding << lsdaEncoding;
    return tuple;
}

souffle::tuple ExceptionDecoder::getCIEPersonality(souffle::Relation *relation,
                                                   const EHP::CIEContents_t *cie)
{
    souffle::tuple tuple(relation);
    tuple << cie->getPosition() << cie->getPersonality() << cie->getPersonalityPointerPosition()
          << cie->getPersonalityPointerSize() << cie->getPersonalityEncoding();
    return tuple;
}

souffle::tuple ExceptionDecoder::getFDE(souffle::Relation *relation, const EHP::FDEContents_t *fde)
{
    souffle::tuple tuple(relation);
    tuple << fde->getPosition() << fde->getLength() << fde->getCIE().getPosition()
          << fde->getStartAddress() << fde->getEndAddress() << fde->getLSDAAddress();
    return tuple;
}

souffle::tuple ExceptionDecoder::getFDEPointerLocations(souffle::Relation *relation,
                                                        const EHP::FDEContents_t *fde)
{
    souffle::tuple tuple(relation);
    tuple << fde->getPosition() << fde->getStartAddressPosition() << fde->getEndAddressPosition()
          << fde->getEndAddressSize() << fde->getLSDAAddressPosition() << fde->getLSDAAddressSize();
    return tuple;
}

souffle::tuple ExceptionDecoder::getEHProgramInstruction(souffle::Relation *relation,
                                                         uint64_t index,
                                                         const EHP::EHProgramInstruction_t *insn,
                                                         const EHP::FDEContents_t *fde)
{
    souffle::tuple tuple(relation);
    tuple << fde->getPosition() << index << insn->getSize();
    auto insnTuple = insn->decode();
    tuple << std::get<0>(insnTuple) << std::get<1>(insnTuple) << std::get<2>(insnTuple);
    return tuple;
}

souffle::tuple ExceptionDecoder::getLSDA(souffle::Relation *relation, const EHP::LSDA_t *lsda,
                                         const EHP::FDEContents_t *fde)
{
    souffle::tuple tuple(relation);
    tuple << fde->getLSDAAddress() << lsda->getCallSiteTableAddress()
          << lsda->getCallSiteTableEncoding() << lsda->getCallSiteTableLength()
          << lsda->getTypeTableAddress() << lsda->getTypeTableEncoding()
          << lsda->getLandingPadBaseAddress();
    return tuple;
}

souffle::tuple ExceptionDecoder::getLSDAPointerLocations(souffle::Relation *relation,
                                                         const EHP::LSDA_t *lsda,
                                                         const EHP::FDEContents_t *fde)
{
    souffle::tuple tuple(relation);
    tuple << fde->getLSDAAddress() << lsda->getTypeTableAddressLocation()
          << lsda->getCallSiteTableAddressLocation();
    return tuple;
}

souffle::tuple ExceptionDecoder::getLSDACallSite(souffle::Relation *relation,
                                                 const EHP::LSDACallSite_t *callSite,
                                                 const EHP::LSDA_t *lsda)
{
    souffle::tuple tuple(relation);
    tuple << lsda->getCallSiteTableAddress() << callSite->getCallSiteAddressPosition()
          << callSite->getCallSiteAddress() << callSite->getCallSiteEndAddressPosition()
          << callSite->getCallSiteEndAddress() << callSite->getLandingPadAddressPosition()
          << callSite->getLandingPadAddress() << callSite->getLandingPadAddressEndPosition();
    return tuple;
}

souffle::tuple ExceptionDecoder::getLSDATypetableEntry(souffle::Relation *relation, uint64_t index,
                                                       const EHP::LSDATypeTableEntry_t *typeEntry,
                                                       const EHP::LSDA_t *lsda)
{
    souffle::tuple tuple(relation);
    tuple << lsda->getTypeTableAddress() << index << typeEntry->getTypeInfoPointer();
    return tuple;
}
