/***** BEGIN LICENSE BLOCK *****

BSD License

Copyright (c) 2005-2015, NIF File Format Library and Tools
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the NIF File Format Library and Tools project may not be
   used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***** END LICENCE BLOCK *****/

#include "nifskope.h"
#include "version.h"
#include "data/nifvalue.h"
#include "model/nifmodel.h"
#include "model/kfmmodel.h"

#include <QApplication>
#include <QColorSpace>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QStack>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QUdpSocket>
#include <QUrl>


QCoreApplication * createApplication( int &argc, char *argv[] )
{
	QCoreApplication::setAttribute( Qt::AA_UseDesktopOpenGL );
	QCoreApplication::setAttribute( Qt::AA_ShareOpenGLContexts );
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	QCoreApplication::setAttribute( Qt::AA_EnableHighDpiScaling );
#endif
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy( Qt::HighDpiScaleFactorRoundingPolicy::PassThrough );
	if ( auto fmt = QSurfaceFormat::defaultFormat(); true ) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
		fmt.setColorSpace( QSurfaceFormat::sRGBColorSpace );
#else
		fmt.setColorSpace( QColorSpace( QColorSpace::SRgb ) );
#endif
		QSurfaceFormat::setDefaultFormat( fmt );
	}

	// Iterate over args
	for ( int i = 1; i < argc; ++i ) {
		// -no-gui: start as core app without all the GUI overhead
		if ( !qstrcmp( argv[i], "-no-gui" ) || !qstrcmp( argv[i], "--no-gui" ) ) {
			return new QCoreApplication( argc, argv );
		}
	}
	return new QApplication( argc, argv );
}


/*
 *  main
 */

//! The main program
int main( int argc, char * argv[] )
{
	QScopedPointer<QCoreApplication> app( createApplication( argc, argv ) );

	if ( auto a = qobject_cast<QApplication *>(app.data()) ) {
		a->setOrganizationName( "NifTools" );
		a->setOrganizationDomain( "niftools.org" );
		a->setApplicationName( "NifSkope " + NifSkopeVersion::rawToMajMin( NIFSKOPE_VERSION ) );
		a->setApplicationVersion( NIFSKOPE_VERSION );
#ifdef NIFSKOPE_REVISION
		a->setApplicationDisplayName( "NifSkope " + NifSkopeVersion::rawToDisplay( NIFSKOPE_VERSION, true ) + " (build " + NIFSKOPE_REVISION + ", " + __DATE__ + ")" );
#else
		a->setApplicationDisplayName( "NifSkope " + NifSkopeVersion::rawToDisplay( NIFSKOPE_VERSION, true ) + " (" + __DATE__ + ")" );
#endif

		// Must set current directory or this causes issues with several features
		QDir::setCurrent( qApp->applicationDirPath() );

		// Register message handler
		//qRegisterMetaType<Message>( "Message" );
		qInstallMessageHandler( NifSkope::MessageOutput );

		// Register types
		qRegisterMetaType<NifValue>( "NifValue" );
#if 0
		QMetaType::registerComparators<NifValue>();
#endif

		// Set locale
		QSettings cfg( QString( "%1/nifskope.ini" ).arg( QCoreApplication::applicationDirPath() ), QSettings::IniFormat );
		cfg.beginGroup( "Settings" );
		NifSkope::SetAppLocale( cfg.value( "Locale", "en" ).toLocale() );
		cfg.endGroup();

		// Load XML files
		NifModel::loadXML();
		KfmModel::loadXML();

		// Init game manager
		(void) Game::GameManager::get();

		int port = NIFSKOPE_IPC_PORT;

		QStack<QString> fnames;

		// Command Line setup
		QCommandLineParser parser;
		parser.addHelpOption();
		parser.addVersionOption();

		// Add port option
		QCommandLineOption portOption( {"p", "port"}, "Port NifSkope listens on", "port" );
		parser.addOption( portOption );

		// Headless batch options — require -no-gui; documented here so they appear in --help
		QCommandLineOption guiConvertToInternalGeomOpt(
			QStringLiteral( "ConvertToInternalGeometry" ),
			QStringLiteral( "Headless: convert external .mesh geometry in Starfield NIF file(s) to internal geometry."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiConvertToInternalGeomOpt );

		QCommandLineOption guiConvertToExternalGeomOpt(
			QStringLiteral( "ConvertToExternalGeometry" ),
			QStringLiteral( "Headless: convert internal geometry in Starfield NIF file(s) to external .mesh files."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place."
				" Uses -o when provided, otherwise falls back to saved GUI export output path." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiConvertToExternalGeomOpt );

		QCommandLineOption guiOutputFolderOpt(
			QStringList{ QStringLiteral( "o" ), QStringLiteral( "OutputFolder" ) },
			QStringLiteral( "Headless: output folder for --ConvertToExternalGeometry .mesh export files."
				" Requires -no-gui." ),
			QStringLiteral( "folder" ) );
		parser.addOption( guiOutputFolderOpt );

		QCommandLineOption guiRemoveUnusedStringsOpt(
			QStringLiteral( "RemoveUnusedStrings" ),
			QStringLiteral( "Headless: remove any unreferenced strings from the NIF header."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiRemoveUnusedStringsOpt );

		QCommandLineOption guiRemoveDuplicateVerticesOpt(
			QStringLiteral( "RemoveDuplicateVertices" ),
			QStringLiteral( "Headless: remove duplicate vertices from all geometry blocks."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place."
				" Warning: for Starfield NIFs this may break any associated morph files." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiRemoveDuplicateVerticesOpt );

		QCommandLineOption guiRemoveUnusedVerticesOpt(
			QStringLiteral( "RemoveUnusedVertices" ),
			QStringLiteral( "Headless: remove unused vertices from all geometry blocks."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place."
				" Warning: for Starfield NIFs this may break any associated morph files." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiRemoveUnusedVerticesOpt );

		QCommandLineOption guiGenerateMeshLODsOpt(
			QStringLiteral( "GenerateMeshLODs" ),
			QStringLiteral( "Headless: generate simplified Starfield mesh LODs for internal BSGeometry blocks."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place."
				" Non-Starfield files are skipped." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiGenerateMeshLODsOpt );

		QCommandLineOption guiOptimizeIndicesOpt(
			QStringLiteral( "OptimizeIndices" ),
			QStringLiteral( "Headless: optimize triangle index ordering for vertex cache efficiency across all geometry blocks."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiOptimizeIndicesOpt );

		QCommandLineOption guiAddTangentSpacesAndUpdateOpt(
			QStringLiteral( "AddTangentSpacesAndUpdate" ),
			QStringLiteral( "Headless: add missing tangent spaces and update tangent/bitangent data where applicable."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiAddTangentSpacesAndUpdateOpt );

		QCommandLineOption guiUpdateBoundsOpt(
			QStringLiteral( "UpdateBounds" ),
			QStringLiteral( "Headless: update bounding spheres/boxes for contained geometry where applicable."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiUpdateBoundsOpt );

		QCommandLineOption guiCombinePropertiesOpt(
			QStringLiteral( "CombineProperties" ),
			QStringLiteral( "Headless: combine duplicate shader properties into one shared block."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiCombinePropertiesOpt );

		QCommandLineOption guiRemoveBogusNodesOpt(
			QStringLiteral( "RemoveBogusNodes" ),
			QStringLiteral( "Headless: remove useless or incorrect block types for the NIF version where applicable."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiRemoveBogusNodesOpt );

		QCommandLineOption guiReorderBlocksOpt(
			QStringLiteral( "ReorderBlocks" ),
			QStringLiteral( "Headless: reorder blocks so the game can properly load them."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiReorderBlocksOpt );

		QCommandLineOption guiSanitizeBeforeSaveOpt(
			QStringLiteral( "SanitizeBeforeSave" ),
			QStringLiteral( "Headless: fix minor errors (for example duplicate block names) before save."
				" Requires -no-gui. Accepts a single .nif file or a folder (recursive). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( guiSanitizeBeforeSaveOpt );

		// Process options
		parser.process( *a );

		// Headless options are not supported in GUI mode
		if ( parser.isSet( guiConvertToInternalGeomOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --ConvertToInternalGeometry requires headless mode. Use: NifSkope -no-gui --ConvertToInternalGeometry <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiConvertToExternalGeomOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --ConvertToExternalGeometry requires headless mode. Use: NifSkope -no-gui --ConvertToExternalGeometry <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiOutputFolderOpt ) ) {
			QTextStream err( stderr );
			err << "Error: -o/--OutputFolder requires headless mode. Use: NifSkope -no-gui --ConvertToExternalGeometry <path> -o <folder>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiRemoveUnusedStringsOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --RemoveUnusedStrings requires headless mode. Use: NifSkope -no-gui --RemoveUnusedStrings <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiRemoveDuplicateVerticesOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --RemoveDuplicateVertices requires headless mode. Use: NifSkope -no-gui --RemoveDuplicateVertices <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiRemoveUnusedVerticesOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --RemoveUnusedVertices requires headless mode. Use: NifSkope -no-gui --RemoveUnusedVertices <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiGenerateMeshLODsOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --GenerateMeshLODs requires headless mode. Use: NifSkope -no-gui --GenerateMeshLODs <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiOptimizeIndicesOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --OptimizeIndices requires headless mode. Use: NifSkope -no-gui --OptimizeIndices <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiAddTangentSpacesAndUpdateOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --AddTangentSpacesAndUpdate requires headless mode. Use: NifSkope -no-gui --AddTangentSpacesAndUpdate <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiUpdateBoundsOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --UpdateBounds requires headless mode. Use: NifSkope -no-gui --UpdateBounds <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiCombinePropertiesOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --CombineProperties requires headless mode. Use: NifSkope -no-gui --CombineProperties <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiRemoveBogusNodesOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --RemoveBogusNodes requires headless mode. Use: NifSkope -no-gui --RemoveBogusNodes <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiReorderBlocksOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --ReorderBlocks requires headless mode. Use: NifSkope -no-gui --ReorderBlocks <path>\n";
			err.flush();
			return 1;
		}
		if ( parser.isSet( guiSanitizeBeforeSaveOpt ) ) {
			QTextStream err( stderr );
			err << "Error: --SanitizeBeforeSave requires headless mode. Use: NifSkope -no-gui --SanitizeBeforeSave <path>\n";
			err.flush();
			return 1;
		}

		// Override port value
		if ( parser.isSet( portOption ) )
			port = parser.value( portOption ).toInt();

		// Files were passed to NifSkope
		for ( const QString & arg : parser.positionalArguments() ) {
			QString fname = QDir::current().filePath( arg );

			if ( QFileInfo( fname ).exists() ) {
				fnames.push( fname );
			}
		}

		// No files were passed to NifSkope, push empty string
		if ( fnames.isEmpty() ) {
			fnames.push( QString() );
		}

		if ( IPCsocket * ipc = IPCsocket::create( port ) ) {
			//qDebug() << "IPCSocket exec";
			ipc->execCommand( QString( "NifSkope::open %1" ).arg( fnames.pop() ) );

			while ( !fnames.isEmpty() ) {
				IPCsocket::sendCommand( QString( "NifSkope::open %1" ).arg( fnames.pop() ), port );
			}

			return a->exec();
		} else {
			//qDebug() << "IPCSocket send";
			while ( !fnames.isEmpty() ) {
				IPCsocket::sendCommand( QString( "NifSkope::open %1" ).arg( fnames.pop() ), port );
			}
			return 0;
		}
	} else {
		// Headless (no-GUI) mode — command line batch tools
		QTextStream out( stdout );
		QTextStream err( stderr );

		QCommandLineParser parser;
		parser.setSingleDashWordOptionMode( QCommandLineParser::ParseAsLongOptions );
		parser.setApplicationDescription(
			QString( "NifSkope %1 — headless batch processing" ).arg( NIFSKOPE_VERSION ) );
		parser.addHelpOption();
		parser.addVersionOption();

		QCommandLineOption noGuiOpt(
			QStringLiteral( "no-gui" ),
			QStringLiteral( "Run in headless mode without launching the GUI." ) );
		parser.addOption( noGuiOpt );

		QCommandLineOption convertToInternalGeomOpt(
			QStringLiteral( "ConvertToInternalGeometry" ),
			QStringLiteral( "Convert all external .mesh geometry in Starfield NIF file(s) to internal geometry."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( convertToInternalGeomOpt );

		QCommandLineOption convertToExternalGeomOpt(
			QStringLiteral( "ConvertToExternalGeometry" ),
			QStringLiteral( "Convert all internal geometry in Starfield NIF file(s) to external .mesh files."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place."
				" Uses -o when provided, otherwise falls back to saved GUI export output path." ),
			QStringLiteral( "path" ) );
		parser.addOption( convertToExternalGeomOpt );

		QCommandLineOption outputFolderOpt(
			QStringList{ QStringLiteral( "o" ), QStringLiteral( "OutputFolder" ) },
			QStringLiteral( "Output folder for --ConvertToExternalGeometry mesh export files."
				" If omitted, saved GUI export output path is used." ),
			QStringLiteral( "folder" ) );
		parser.addOption( outputFolderOpt );

		QCommandLineOption removeUnusedStringsOpt(
			QStringLiteral( "RemoveUnusedStrings" ),
			QStringLiteral( "Remove any unreferenced strings from the NIF header."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( removeUnusedStringsOpt );

		QCommandLineOption removeDuplicateVerticesOpt(
			QStringLiteral( "RemoveDuplicateVertices" ),
			QStringLiteral( "Remove duplicate vertices from all geometry blocks (BSTriShape, BSGeometry, NiTriShape, etc.)."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place."
				" Warning: for Starfield NIFs this may break any associated morph files." ),
			QStringLiteral( "path" ) );
		parser.addOption( removeDuplicateVerticesOpt );

		QCommandLineOption removeUnusedVerticesOpt(
			QStringLiteral( "RemoveUnusedVertices" ),
			QStringLiteral( "Remove unused vertices from all geometry blocks (BSTriShape, BSGeometry, NiTriShape, etc.)."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place."
				" Warning: for Starfield NIFs this may break any associated morph files." ),
			QStringLiteral( "path" ) );
		parser.addOption( removeUnusedVerticesOpt );

		QCommandLineOption generateMeshLODsOpt(
			QStringLiteral( "GenerateMeshLODs" ),
			QStringLiteral( "Generate Starfield mesh LODs for internal BSGeometry blocks."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place."
				" Non-Starfield files (BSVersion < 170) are skipped without modification." ),
			QStringLiteral( "path" ) );
		parser.addOption( generateMeshLODsOpt );

		QCommandLineOption optimizeIndicesOpt(
			QStringLiteral( "OptimizeIndices" ),
			QStringLiteral( "Optimize triangle index ordering for vertex cache efficiency across all geometry blocks"
				" (BSTriShape, BSGeometry, NiTriShape, etc.)."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( optimizeIndicesOpt );

		QCommandLineOption addTangentSpacesAndUpdateOpt(
			QStringLiteral( "AddTangentSpacesAndUpdate" ),
			QStringLiteral( "Add missing tangent spaces and update tangent/bitangent data where applicable."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( addTangentSpacesAndUpdateOpt );

		QCommandLineOption updateBoundsOpt(
			QStringLiteral( "UpdateBounds" ),
			QStringLiteral( "Update bounding spheres/boxes for contained geometry where applicable."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( updateBoundsOpt );

		QCommandLineOption combinePropertiesOpt(
			QStringLiteral( "CombineProperties" ),
			QStringLiteral( "Combine duplicate shader properties into a single shared block."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( combinePropertiesOpt );

		QCommandLineOption removeBogusNodesOpt(
			QStringLiteral( "RemoveBogusNodes" ),
			QStringLiteral( "Remove useless or incorrect block types for the NIF version where applicable."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( removeBogusNodesOpt );

		QCommandLineOption reorderBlocksOpt(
			QStringLiteral( "ReorderBlocks" ),
			QStringLiteral( "Reorder blocks so the game can properly load them."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( reorderBlocksOpt );

		QCommandLineOption sanitizeBeforeSaveOpt(
			QStringLiteral( "SanitizeBeforeSave" ),
			QStringLiteral( "Fix minor errors (for example duplicate block names) before save."
				" Accepts a single .nif file or a folder (processed recursively). Files are overwritten in-place." ),
			QStringLiteral( "path" ) );
		parser.addOption( sanitizeBeforeSaveOpt );

		parser.process( *app );

		if ( parser.isSet( outputFolderOpt ) && !parser.isSet( convertToExternalGeomOpt ) ) {
			err << "-o/--OutputFolder can only be used with --ConvertToExternalGeometry\n";
			err.flush();
			return 1;
		}

		if ( !( parser.isSet( convertToInternalGeomOpt ) || parser.isSet( convertToExternalGeomOpt ) || parser.isSet( removeUnusedStringsOpt ) || parser.isSet( removeDuplicateVerticesOpt ) || parser.isSet( removeUnusedVerticesOpt ) || parser.isSet( generateMeshLODsOpt ) || parser.isSet( optimizeIndicesOpt ) || parser.isSet( addTangentSpacesAndUpdateOpt ) || parser.isSet( updateBoundsOpt ) || parser.isSet( combinePropertiesOpt ) || parser.isSet( removeBogusNodesOpt ) || parser.isSet( reorderBlocksOpt ) || parser.isSet( sanitizeBeforeSaveOpt ) ) ) {
			err << "NifSkope headless mode: no actionable option specified.\n\n";
			err << parser.helpText();
			err.flush();
			return 1;
		}

		// Load NIF/XML schema (required before any NifModel usage)
		QDir::setCurrent( qApp->applicationDirPath() );
		NifModel::loadXML();
		KfmModel::loadXML();

		// Collect .nif files from the given path (file or directory)
		auto collectNifFiles = []( const QString & path ) -> QStringList {
			QStringList files;
			QFileInfo fi( path );
			if ( fi.isFile() ) {
				if ( path.endsWith( QLatin1String( ".nif" ), Qt::CaseInsensitive ) )
					files.append( QDir::fromNativeSeparators( path ) );
			} else if ( fi.isDir() ) {
				QDirIterator it( path, QStringList{ QStringLiteral( "*.nif" ) },
								QDir::Files | QDir::Readable,
								QDirIterator::Subdirectories );
				while ( it.hasNext() )
					files.append( QDir::fromNativeSeparators( it.next() ) );
			}
			return files;
		};

		if ( parser.isSet( convertToInternalGeomOpt ) ) {
			const QString inputPath = parser.value( convertToInternalGeomOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "ConvertToInternalGeometry: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					bool modified = nif.convertToInternalGeometry();
					if ( modified ) {
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
						out << "  Saved (geometry converted)\n";
					} else {
						out << "  Skipped (already internal or not Starfield)\n";
					}
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( convertToExternalGeomOpt ) ) {
			const QString inputPath = parser.value( convertToExternalGeomOpt );
			const QStringList fileList = collectNifFiles( inputPath );
			QString outputFolder;
			bool useOutputFolderOverride = false;
			if ( parser.isSet( outputFolderOpt ) ) {
				outputFolder = QDir::fromNativeSeparators( parser.value( outputFolderOpt ) ).trimmed();
				if ( outputFolder.isEmpty() ) {
					err << "ConvertToExternalGeometry: output folder cannot be empty\n";
					err.flush();
					return 1;
				}
				QDir outDir( outputFolder );
				if ( !outDir.exists() && !QDir().mkpath( outputFolder ) ) {
					err << "ConvertToExternalGeometry: failed to create output folder '" << outputFolder << "'\n";
					err.flush();
					return 1;
				}
				if ( !QFileInfo( outputFolder ).isDir() ) {
					err << "ConvertToExternalGeometry: output path is not a directory: '" << outputFolder << "'\n";
					err.flush();
					return 1;
				}
				useOutputFolderOverride = true;
			}

			if ( fileList.isEmpty() ) {
				err << "ConvertToExternalGeometry: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					bool modified = useOutputFolderOverride
						? nif.convertToExternalGeometry( outputFolder )
						: nif.convertToExternalGeometry();
					if ( modified ) {
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
						out << "  Saved (geometry converted)\n";
					} else {
						out << "  Skipped (already external or not Starfield)\n";
					}
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					if ( !useOutputFolderOverride )
						err << "  Hint: pass -o <folder> or set the output path in GUI using Mesh -> Convert to External Geometry first.\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( removeUnusedStringsOpt ) ) {
			const QString inputPath = parser.value( removeUnusedStringsOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "RemoveUnusedStrings: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					bool modified = nif.removeUnusedStrings();
					if ( modified ) {
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
						out << "  Saved (unused strings removed)\n";
					} else {
						out << "  Skipped (no unused strings)\n";
					}
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( removeDuplicateVerticesOpt ) ) {
			const QString inputPath = parser.value( removeDuplicateVerticesOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "RemoveDuplicateVertices: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			err << "Warning: --RemoveDuplicateVertices may break Starfield NIF files that reference morph files."
				" No morph file check is performed.\n";
			err.flush();

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.removeDuplicateVertices();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (duplicate vertices removed)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( removeUnusedVerticesOpt ) ) {
			const QString inputPath = parser.value( removeUnusedVerticesOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "RemoveUnusedVertices: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			err << "Warning: --RemoveUnusedVertices may break Starfield NIF files that reference morph files."
				" No morph file check is performed.\n";
			err.flush();

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.removeUnusedVertices();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (unused vertices removed)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( generateMeshLODsOpt ) ) {
			const QString inputPath = parser.value( generateMeshLODsOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "GenerateMeshLODs: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					if ( nif.getBSVersion() < 170 ) {
						out << "  Skipped (Starfield only)\n";
						out.flush();
						continue;
					}
					nif.generateMeshLODs();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (LODs generated)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( optimizeIndicesOpt ) ) {
			const QString inputPath = parser.value( optimizeIndicesOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "OptimizeIndices: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.optimizeIndices();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (indices optimized)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( addTangentSpacesAndUpdateOpt ) ) {
			const QString inputPath = parser.value( addTangentSpacesAndUpdateOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "AddTangentSpacesAndUpdate: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.addTangentSpacesAndUpdate();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (tangent spaces added/updated)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( updateBoundsOpt ) ) {
			const QString inputPath = parser.value( updateBoundsOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "UpdateBounds: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.updateBounds();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (bounds updated)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( combinePropertiesOpt ) ) {
			const QString inputPath = parser.value( combinePropertiesOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "CombineProperties: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.combineProperties();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (properties combined)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( removeBogusNodesOpt ) ) {
			const QString inputPath = parser.value( removeBogusNodesOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "RemoveBogusNodes: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.removeBogusNodes();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (bogus nodes removed)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( reorderBlocksOpt ) ) {
			const QString inputPath = parser.value( reorderBlocksOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "ReorderBlocks: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.reorderBlocks();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (blocks reordered)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}

		if ( parser.isSet( sanitizeBeforeSaveOpt ) ) {
			const QString inputPath = parser.value( sanitizeBeforeSaveOpt );
			const QStringList fileList = collectNifFiles( inputPath );

			if ( fileList.isEmpty() ) {
				err << "SanitizeBeforeSave: no .nif files found at '" << inputPath << "'\n";
				err.flush();
				return 1;
			}

			int exitCode = 0;
			for ( const QString & filePath : fileList ) {
				out << "Processing: " << filePath << "\n";
				out.flush();
				try {
					NifModel nif;
					nif.setBatchProcessingMode( true );
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::ReadOnly ) ) {
							err << "  Error: cannot open file for reading\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						std::string tmp( filePath.toStdString() );
						nif.load( f, tmp.c_str() );
					}
					if ( !nif.getMessages().isEmpty() ) {
						err << "  Error: failed to parse NIF data\n";
						err.flush();
						exitCode = 1;
						continue;
					}
					nif.sanitizeBeforeSave();
					{
						QFile f( filePath );
						if ( !f.open( QIODevice::WriteOnly ) ) {
							err << "  Error: cannot open file for writing\n";
							err.flush();
							exitCode = 1;
							continue;
						}
						nif.save( f );
					}
					out << "  Saved (sanitized before save)\n";
					out.flush();
				} catch ( std::exception & e ) {
					err << "  Error: " << e.what() << "\n";
					err.flush();
					exitCode = 1;
				}
			}
			return exitCode;
		}
	}

	return 0;
}



/*
*  IPC socket
*/

IPCsocket * IPCsocket::create( int port )
{
	QUdpSocket * udp = new QUdpSocket();

	if ( udp->bind( QHostAddress( QHostAddress::LocalHost ), port, QUdpSocket::DontShareAddress ) ) {
		IPCsocket * ipc = new IPCsocket( udp );
		QDesktopServices::setUrlHandler( "nif", ipc, "openNif" );
		return ipc;
	}

	return nullptr;
}

void IPCsocket::sendCommand( const QString & cmd, int port )
{
	QUdpSocket udp;
	udp.writeDatagram( (const char *)cmd.data(), cmd.length() * sizeof( QChar ), QHostAddress( QHostAddress::LocalHost ), port );
}

IPCsocket::IPCsocket( QUdpSocket * s ) : QObject(), socket( s )
{
	QObject::connect( socket, &QUdpSocket::readyRead, this, &IPCsocket::processDatagram );
}

IPCsocket::~IPCsocket()
{
	delete socket;
}

void IPCsocket::processDatagram()
{
	while ( socket->hasPendingDatagrams() ) {
		QByteArray data;
		data.resize( socket->pendingDatagramSize() );
		QHostAddress host;
		quint16 port = 0;

		socket->readDatagram( data.data(), data.size(), &host, &port );

		if ( host == QHostAddress( QHostAddress::LocalHost ) && (data.size() % sizeof( QChar )) == 0 ) {
			QString cmd;
			cmd.setUnicode( (QChar *)data.data(), data.size() / sizeof( QChar ) );
			execCommand( cmd );
		}
	}
}

void IPCsocket::execCommand( const QString & cmd )
{
	if ( cmd.startsWith( "NifSkope::open" ) ) {
		openNif( cmd.right( cmd.length() - 15 ) );
	}
}

void IPCsocket::openNif( const QUrl & url )
{
	auto file = url.toString();
	file.remove( 0, 4 );

	openNif( file );
}

void IPCsocket::openNif( const QString & url )
{
	NifSkope::createWindow( url );
}
