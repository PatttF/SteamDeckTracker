#include "FontConfig.h"
#include "System/Console/Trace.h"
#include "Externals/TinyXML/tinyxml.h"
#include "Application/Model/Config.h"

unsigned char font[8*8*128]= {
	#include "Resources/earth.txt"
};

const unsigned char embeddedFontBank[FONT_TYPE_COUNT][8*8*128]= {
	{//0 original
		#include "Resources/original.txt"
	},
	{//1 digital
		#include "Resources/digital.txt"
	},
	{//2 monster
		#include "Resources/monster.txt"
	},
	{//3 earth
		#include "Resources/earth.txt"
	}
};

void FontConfig(){
    const char *fontType=Config::GetInstance()->GetValue("FONTTYPE") ;
	if (fontType)
	{
		if(strcmp(fontType, "CUSTOM") == 0 || strcmp(fontType, "custom") == 0){
			// Load custom font from `root:` so users can place custom_font.xml in Documents/SDTracker
			Path path("root:custom_font.xml") ;
			TiXmlDocument document(path.GetPath());
			bool loadOkay = document.LoadFile();
			if (!loadOkay) {
				
			}
			else{
				TiXmlNode* rootnode = 0;
				rootnode = document.FirstChild( "FONT" );
				if (!rootnode)  {
					Trace::Error("No master node for custom_font") ;
				}
				else{
					TiXmlElement *rootelement = rootnode->ToElement();
					TiXmlNode *node = rootelement->FirstChildElement() ;
					if (node) {
						TiXmlElement *element = node->ToElement();
						int j = 0;
						const int fontSize = 8*8*128;
						while (element) {
							const char *elem=element->Value() ; // sould be DATA but we don't really care
							const char *font_data=element->Attribute("value") ;
							if (font_data) {
								int i = 0;
								while (font_data[i] != '\0' && j < fontSize) {
									if (font_data[i] == '1') {
										font[j++] = 1;
									} else if (font_data[i] == '0') {
										font[j++] = 0;
									}
									i++;
								}
							}
							element = element->NextSiblingElement();
						}
					}
				}
			}
		}
		else if(0 <= atoi(fontType) && atoi(fontType) < FONT_TYPE_COUNT){
			int ft = atoi(fontType);
			if (ft == 3) {
				// If 'earth' font type is selected, prefer a root:custom_font.xml if present
				Path path("root:custom_font.xml") ;
				TiXmlDocument document(path.GetPath());
				bool loadOkay = document.LoadFile();
				if (loadOkay) {
					TiXmlNode* rootnode = document.FirstChild( "FONT" );
					if (!rootnode)  {
						Trace::Error("No master node for custom_font") ;
						// fallback to embedded
						for(int i=0;i<8*8*(FONT_COUNT+1);i++) font[i]=embeddedFontBank[ft][i];
					}
					else{
						TiXmlElement *rootelement = rootnode->ToElement();
						TiXmlNode *node = rootelement->FirstChildElement() ;
						if (node) {
							TiXmlElement *element = node->ToElement();
							int j = 0;
							const int fontSize = 8*8*128;
							while (element) {
								const char *font_data=element->Attribute("value") ;
								if (font_data) {
									int i = 0;
									while (font_data[i] != '\0' && j < fontSize) {
										if (font_data[i] == '1') {
											font[j++] = 1;
										} else if (font_data[i] == '0') {
											font[j++] = 0;
										}
										i++;
									}
								}
								element = element->NextSiblingElement();
							}
						}
					}
				}
				else {
					for(int i=0;i<8*8*(FONT_COUNT+1);i++)
					{
						font[i]=embeddedFontBank[ft][i];
					}
				}
			}
			else {
				for(int i=0;i<8*8*(FONT_COUNT+1);i++)
				{
					font[i]=embeddedFontBank[ft][i];
				}
			}
		}
	}
}