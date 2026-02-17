#include "PersistencyService.h"
#include "Persistent.h"
#include "Externals/Compression/lz.h"
#include "System/Console/Trace.h"
#include "Foundation/Types/Types.h"

PersistencyService::PersistencyService():Service(MAKE_FOURCC('S','V','P','S')) {
} ;

void PersistencyService::Save(const char *name) {

    Trace::Debug("SAVE: PersistencyService::Save(%s)", name);
    Path filename(name);

    TiXmlDocument doc(filename.GetPath());
    TiXmlElement first("LITTLEGPTRACKER") ;
	TiXmlNode *node=doc.InsertEndChild(first) ;

	// Loop on all registered service
	// accumulating XML flow
	
	int serviceIdx = 0;
	IteratorPtr<SubService> it(GetIterator()) ;
	for (it->Begin();!it->IsDone();it->Next()) {
		Persistent *currentItem=(Persistent *)&it->CurrentItem() ;
		Trace::Debug("SAVE: Saving service %d: %s", serviceIdx, currentItem->GetNodeName());
		currentItem->Save(node) ;
		Trace::Debug("SAVE: Service %d done", serviceIdx);
		serviceIdx++;
	} ;

	Trace::Debug("SAVE: Writing file...");
	doc.SaveFile() ;
	Trace::Debug("SAVE: File written successfully");
};

bool PersistencyService::Load() {

	Path filename("project:lgptsav.dat") ;
	PersistencyDocument doc( filename.GetPath() );

	// Try opening the file
	
	FileSystem *fs=FileSystem::GetInstance() ;
	I_File *file=fs->Open(filename.GetPath().c_str(),"r") ;
	if (!file) return false ;
	
	// get file size and read all buffer
	
	file->Seek(0,SEEK_END) ;
	int length=file->Tell() ;

	// Allocate one extra byte to ensure the buffer is NUL-terminated for TinyXML
	unsigned char *compBuffer=(unsigned char *)SYS_MALLOC(length + 1) ;

  file->Seek(0,SEEK_SET) ;
	int read = file->Read(compBuffer,1,length) ;
	file->Close();
	delete file ;

	// Ensure NUL-termination even if file was shorter than expected
	if (read < length) {
		compBuffer[read] = '\0';
	} else {
		compBuffer[length] = '\0';
	}

	if (!doc.Parse((char *)compBuffer)) {
        
		// Get uncompressed buffer size from first byte
		
		int offset=sizeof(int) ;
		int fullLength ;
		memcpy(&fullLength,compBuffer,offset) ;
		
		// Sanity check: reject obviously bad lengths
		if (fullLength <= 0 || fullLength > 16 * 1024 * 1024) {
			Trace::Error("Corrupt save: bad uncompressed size %d", fullLength) ;
			SYS_FREE(compBuffer);
			return false ;
		}

		// Allocate a buffer to decompress data (extra byte for NUL)
		
		unsigned char *xmlSource=(unsigned char *)SYS_MALLOC(fullLength + 1) ;
		if (!xmlSource) {
			Trace::Error("could not allocate space for %d bytes", fullLength) ;
			SYS_FREE(compBuffer);
			return false ;
		}

    LZ_Uncompress(compBuffer+offset,xmlSource,length-offset);

		// Ensure decompressed buffer is NUL-terminated
		xmlSource[fullLength] = '\0';

		// Initialize XML document on decompressed buffer
		doc.Parse((char *)xmlSource) ;

		SYS_FREE(xmlSource) ;

		
	} ; 
	SYS_FREE(compBuffer) ;

	TiXmlNode* node = 0;
	node = doc.FirstChild( "LITTLEGPTRACKER" );
	if (!node) {
		Trace::Error("could not find master node") ;
		return false ;
	};

	TiXmlElement* element =node->ToElement();
	node = element->FirstChildElement() ;
	if (node) {
		element = node->ToElement();
		while (element) {
			IteratorPtr<SubService> it(GetIterator()) ;
			for (it->Begin();!it->IsDone();it->Next()) {
				Persistent *currentItem=(Persistent *)&it->CurrentItem() ;
				if (currentItem->Restore(element)) {
					break ;
				} ;
			}
			element = element->NextSiblingElement();
		} ;
	}
	return true ;
} ;
