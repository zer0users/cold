#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fstream>
#include <algorithm>
#include <map>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace fs = std::filesystem;

class ColdVM {
private:
    std::string diskDir;
    std::string romPath;
    std::string firmwarePath;
    std::string varsPath;
    std::string noVNCPath;
    bool useVNC;
    bool useBridge;
    std::string bridgeInterface;
    pid_t qemuPid;
    pid_t websockifyPid;
    std::vector<std::string> diskFiles;
    std::vector<std::string> isoFiles;
    
    // Configuración mejorada
    int cpuCores;
    int ramGB;
    std::string cpuModel;
    bool enableCamera;
    bool enableAudio;
    bool enableMicrophone;

public:
    ColdVM() {
        diskDir = "./devices/disk";
        romPath = "./devices/rom";
        firmwarePath = "./boot/firmware/OVMF_CODE.fd";
        varsPath = "./boot/firmware/OVMF_VARS.fd";
        noVNCPath = "./libraries/noVNC";
        useVNC = true;
        useBridge = true;
        bridgeInterface = "virbr0";
        qemuPid = -1;
        websockifyPid = -1;
        
        // Configuración por defecto
        cpuCores = 4;
        ramGB = 4;
        cpuModel = "host";
        enableCamera = true;
        enableAudio = true;
        enableMicrophone = true;
    }

    // Sistema de logs mejorado
    void log(const std::string& message) {
        std::cout << "- " << message << std::endl;
    }

    void warning(const std::string& message) {
        std::cout << "! " << message << std::endl;
    }

    void debug(const std::string& message) {
        std::cout << "+ " << message << std::endl;
    }

    void error(const std::string& message) {
        std::cerr << "✗ " << message << std::endl;
    }

    void success(const std::string& message) {
        std::cout << "✓ " << message << std::endl;
    }

    bool checkFile(const std::string& path, const std::string& name) {
        if (fs::exists(path)) {
            success(name + " found!");
            return true;
        } else {
            warning(name + " not found at: " + path);
            return false;
        }
    }

    bool checkCommand(const std::string& cmd, const std::string& name) {
        std::string checkCmd = "which " + cmd + " > /dev/null 2>&1";
        if (system(checkCmd.c_str()) == 0) {
            success(name + " is available!");
            return true;
        } else {
            warning(name + " is not installed!");
            return false;
        }
    }

    bool checkBridgeInterface() {
        std::string cmd = "ip link show " + bridgeInterface + " > /dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            success("Bridge interface '" + bridgeInterface + "' is available!");
            return true;
        } else {
            warning("Bridge interface '" + bridgeInterface + "' not found!");
            warning("Falling back to user networking (NAT)");
            useBridge = false;
            return false;
        }
    }

    std::vector<std::string> findAllDisks() {
        debug("Scanning for disk images...");
        std::vector<std::string> disks;
        try {
            if (fs::exists(diskDir) && fs::is_directory(diskDir)) {
                for (const auto& entry : fs::directory_iterator(diskDir)) {
                    std::string ext = entry.path().extension().string();
                    if (ext == ".qcow2" || ext == ".img" || ext == ".raw" || ext == ".vdi" || ext == ".vmdk") {
                        disks.push_back(entry.path().string());
                        debug("Found disk: " + entry.path().filename().string());
                    }
                }
                std::sort(disks.begin(), disks.end());
            }
        } catch (const fs::filesystem_error& e) {
            error("Failed to scan disk directory: " + std::string(e.what()));
        }
        return disks;
    }

    std::vector<std::string> findAllISOs() {
        debug("Scanning for ISO files...");
        std::vector<std::string> isos;
        try {
            if (fs::exists(romPath) && fs::is_directory(romPath)) {
                for (const auto& entry : fs::directory_iterator(romPath)) {
                    if (entry.path().extension() == ".iso") {
                        isos.push_back(entry.path().string());
                        debug("Found ISO: " + entry.path().filename().string());
                    }
                }
                std::sort(isos.begin(), isos.end());
            }
        } catch (const fs::filesystem_error& e) {
            error("Failed to scan ROM directory: " + std::string(e.what()));
        }
        return isos;
    }

    void createDirectories() {
        debug("Creating required directories...");
        try {
            fs::create_directories(diskDir);
            fs::create_directories(romPath);
            fs::create_directories("./boot/firmware");
            fs::create_directories("./libraries");
            success("Directory structure created!");
        } catch (const fs::filesystem_error& e) {
            error("Failed to create directories: " + std::string(e.what()));
        }
    }

    bool createDefaultDisk() {
        std::string defaultDiskPath = diskDir + "/disk.qcow2";
        if (!fs::exists(defaultDiskPath)) {
            log("Creating default 30GB disk image...");
            std::string cmd = "qemu-img create -f qcow2 \"" + defaultDiskPath + "\" 30G 2>&1";
            int result = system(cmd.c_str());
            if (result == 0) {
                success("Default disk created successfully!");
                return true;
            } else {
                error("Failed to create default disk!");
                return false;
            }
        }
        return true;
    }

    bool createVarsFile() {
        if (!fs::exists(varsPath)) {
            log("Creating OVMF VARS file...");
            
            // Intentar copiar desde ubicaciones comunes
            std::vector<std::string> varsSources = {
                "/usr/share/OVMF/OVMF_VARS.fd",
                "/usr/share/edk2-ovmf/x64/OVMF_VARS.fd",
                "/usr/share/qemu/OVMF_VARS.fd"
            };
            
            for (const auto& source : varsSources) {
                if (fs::exists(source)) {
                    try {
                        fs::copy_file(source, varsPath);
                        success("OVMF VARS file created from system template!");
                        return true;
                    } catch (const fs::filesystem_error& e) {
                        debug("Failed to copy from " + source);
                    }
                }
            }
            
            // Si no se encuentra, crear uno vacío
            warning("Creating empty OVMF VARS file (not recommended)");
            std::ofstream varsFile(varsPath, std::ios::binary);
            std::vector<char> emptyVars(64 * 1024 * 1024, 0);
            varsFile.write(emptyVars.data(), emptyVars.size());
            varsFile.close();
            return true;
        }
        return true;
    }

    std::vector<std::string> buildQEMUCommand() {
        std::vector<std::string> cmd;
        
        cmd.push_back("qemu-system-x86_64");
        
        // Aceleración KVM
        cmd.push_back("-enable-kvm");
        
        // CPU
        cmd.push_back("-cpu");
        cmd.push_back(cpuModel);
        cmd.push_back("-smp");
        cmd.push_back(std::to_string(cpuCores));
        
        // RAM
        cmd.push_back("-m");
        cmd.push_back(std::to_string(ramGB) + "G");
        
        // VirtIO GPU para mejor rendimiento
        cmd.push_back("-vga");
        cmd.push_back("virtio");
        cmd.push_back("-display");
        if (useVNC) {
            cmd.push_back("none");
            cmd.push_back("-vnc");
            cmd.push_back(":1");
        } else {
            cmd.push_back("gtk,gl=on");
        }
        
        // UEFI Firmware (OVMF)
        if (fs::exists(firmwarePath)) {
            cmd.push_back("-drive");
            cmd.push_back("if=pflash,format=raw,readonly=on,file=" + firmwarePath);
            
            if (createVarsFile()) {
                cmd.push_back("-drive");
                cmd.push_back("if=pflash,format=raw,file=" + varsPath);
            }
        }
        
        // Discos
        if (!diskFiles.empty()) {
            log("Attaching " + std::to_string(diskFiles.size()) + " disk(s):");
            for (size_t i = 0; i < diskFiles.size(); i++) {
                std::string diskPath = diskFiles[i];
                std::string format = "qcow2";
                
                if (diskPath.find(".img") != std::string::npos || 
                    diskPath.find(".raw") != std::string::npos) {
                    format = "raw";
                } else if (diskPath.find(".vdi") != std::string::npos) {
                    format = "vdi";
                } else if (diskPath.find(".vmdk") != std::string::npos) {
                    format = "vmdk";
                }
                
                cmd.push_back("-drive");
                cmd.push_back("file=" + diskPath + ",format=" + format + ",if=virtio,cache=writeback");
                
                std::string bootFlag = (i == 0) ? " [PRIMARY BOOT]" : "";
                log("  → " + fs::path(diskPath).filename().string() + bootFlag);
            }
        }
        
        // ISOs
        if (!isoFiles.empty()) {
            log("Attaching " + std::to_string(isoFiles.size()) + " ISO(s):");
            for (size_t i = 0; i < isoFiles.size(); i++) {
                std::string isoPath = isoFiles[i];
                
                if (i == 0) {
                    cmd.push_back("-cdrom");
                    cmd.push_back(isoPath);
                    log("  → " + fs::path(isoPath).filename().string() + " [CDROM - BOOT PRIORITY]");
                } else {
                    cmd.push_back("-drive");
                    cmd.push_back("file=" + isoPath + ",media=cdrom,readonly=on,if=ide,index=" + std::to_string(i));
                    log("  → " + fs::path(isoPath).filename().string() + " [CDROM " + std::to_string(i) + "]");
                }
            }
        }
        
        // Audio con ALSA
        if (enableAudio) {
            cmd.push_back("-audiodev");
            cmd.push_back("alsa,id=audio0");
            cmd.push_back("-device");
            cmd.push_back("intel-hda");
            
            if (enableMicrophone) {
                cmd.push_back("-device");
                cmd.push_back("hda-duplex,audiodev=audio0");
                success("Audio & Microphone enabled!");
            } else {
                cmd.push_back("-device");
                cmd.push_back("hda-output,audiodev=audio0");
                success("Audio enabled (no microphone)");
                warning("Microphone is disabled!");
            }
        } else {
            warning("Audio is disabled!");
        }
        
        // Red con bridge o NAT
        if (useBridge) {
            cmd.push_back("-netdev");
            cmd.push_back("bridge,id=net0,br=" + bridgeInterface);
            cmd.push_back("-device");
            cmd.push_back("virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56");
            success("Network: Bridge mode (" + bridgeInterface + ") with internet access!");
        } else {
            cmd.push_back("-netdev");
            cmd.push_back("user,id=net0");
            cmd.push_back("-device");
            cmd.push_back("virtio-net-pci,netdev=net0");
            success("Network: NAT mode with internet access!");
        }
        
        // USB Controller y dispositivos
        cmd.push_back("-device");
        cmd.push_back("qemu-xhci,id=xhci");
        
        // Tablet para mejor precisión del mouse
        cmd.push_back("-device");
        cmd.push_back("usb-tablet");
        
        // Cámara web (USB passthrough)
        if (enableCamera) {
            debug("Detecting USB camera devices...");
            
            // Ejecutar lsusb y buscar cámaras comunes
            FILE* pipe = popen("lsusb 2>/dev/null", "r");
            if (pipe) {
                char buffer[256];
                bool cameraFound = false;
                std::string cameraVendor, cameraProduct, cameraName;
                
                while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    std::string line(buffer);
                    
                    // Buscar palabras clave de cámaras
                    if (line.find("Camera") != std::string::npos || 
                        line.find("Webcam") != std::string::npos ||
                        line.find("HD Webcam") != std::string::npos ||
                        line.find("Integrated Camera") != std::string::npos) {
                        
                        // Extraer vendor:product ID
                        size_t idPos = line.find("ID ");
                        if (idPos != std::string::npos) {
                            std::string ids = line.substr(idPos + 3, 9); // "xxxx:yyyy"
                            cameraVendor = ids.substr(0, 4);
                            cameraProduct = ids.substr(5, 4);
                            
                            // Extraer nombre
                            size_t namePos = line.find(ids) + 10;
                            cameraName = line.substr(namePos);
                            cameraName.erase(cameraName.find_last_not_of(" \n\r\t") + 1);
                            
                            cameraFound = true;
                            break;
                        }
                    }
                }
                pclose(pipe);
                
                if (cameraFound) {
                    // Usar USB passthrough con los IDs detectados
                    cmd.push_back("-device");
                    cmd.push_back("usb-host,vendorid=0x" + cameraVendor + ",productid=0x" + cameraProduct);
                    success("Camera enabled: " + cameraName);
                    debug("Camera IDs: " + cameraVendor + ":" + cameraProduct);
                } else {
                    warning("No camera device found! Camera disabled.");
                    warning("Make sure your camera is connected and working");
                }
            } else {
                warning("Could not execute lsusb to detect camera!");
            }
        } else {
            warning("Camera is disabled!");
        }
        
        // RTC
        cmd.push_back("-rtc");
        cmd.push_back("base=localtime,clock=host,driftfix=slew");
        
        // Boot order
        if (!isoFiles.empty()) {
            cmd.push_back("-boot");
            cmd.push_back("order=dc,menu=on");
        } else {
            cmd.push_back("-boot");
            cmd.push_back("order=c,menu=on");
        }
        
        // Mejoras de rendimiento
        cmd.push_back("-machine");
        cmd.push_back("type=q35,accel=kvm");
        
        return cmd;
    }

    bool startWebsockify() {
        if (!useVNC) return true;
        
        log("Starting websockify for noVNC...");
        
        if (!fs::exists(noVNCPath)) {
            error("noVNC directory not found at: " + noVNCPath);
            return false;
        }
        
        pid_t pid = fork();
        if (pid == 0) {
            std::string cmd = "websockify --web=" + noVNCPath + " 8080 localhost:5901 2>&1";
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
            exit(1);
        } else if (pid > 0) {
            websockifyPid = pid;
            sleep(2);
            return true;
        } else {
            error("Failed to fork websockify process!");
            return false;
        }
    }

    bool startQEMU() {
        log("Starting QEMU virtual machine...");
        
        auto cmd = buildQEMUCommand();
        
        // Mostrar comando completo en debug
        debug("QEMU Command:");
        std::string fullCmd = "";
        for (const auto& arg : cmd) {
            fullCmd += arg + " ";
        }
        debug(fullCmd);
        
        std::vector<char*> args;
        for (const auto& arg : cmd) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        
        pid_t pid = fork();
        if (pid == 0) {
            execvp(args[0], args.data());
            exit(1);
        } else if (pid > 0) {
            qemuPid = pid;
            sleep(3);
            return true;
        } else {
            error("Failed to fork QEMU process!");
            return false;
        }
    }

    void cleanup() {
        log("Shutting down Cold VM...");
        if (qemuPid != -1) {
            kill(qemuPid, SIGTERM);
            waitpid(qemuPid, nullptr, 0);
            success("QEMU stopped");
        }
        if (websockifyPid != -1) {
            kill(websockifyPid, SIGTERM);
            waitpid(websockifyPid, nullptr, 0);
            success("Websockify stopped");
        }
    }

    void printHeader() {
        std::cout << "\n";
        std::cout << "╔═══════════════════════════════════════╗\n";
        std::cout << "║          COLD VM MANAGER v2.0         ║\n";
        std::cout << "║     Advanced Virtual Machine System   ║\n";
        std::cout << "╚═══════════════════════════════════════╝\n";
        std::cout << "\n";
    }

    void printConfiguration() {
        log("System Configuration:");
        std::cout << "  → CPU: " << cpuModel << " (" << cpuCores << " cores)\n";
        std::cout << "  → RAM: " << ramGB << " GB\n";
        std::cout << "  → VirtIO: Enabled\n";
        std::cout << "  → OVMF/UEFI: " << (fs::exists(firmwarePath) ? "Enabled" : "Disabled") << "\n";
        std::cout << "  → Display: " << (useVNC ? "VNC (Remote)" : "GTK (Local)") << "\n";
        std::cout << "\n";
    }

    bool boot() {
        printHeader();
        log("Initializing Cold VM...");
        
        createDirectories();
        
        debug("Checking system requirements...");
        
        // Verificar QEMU
        if (!checkCommand("qemu-system-x86_64", "QEMU")) {
            error("QEMU is required but not installed!");
            return false;
        }
        
        // Verificar firmware
        checkFile(firmwarePath, "OVMF Firmware");
        
        // Verificar componentes VNC
        if (useVNC) {
            if (!checkCommand("websockify", "Websockify")) {
                error("Websockify is required for VNC mode!");
                return false;
            }
            checkFile(noVNCPath, "noVNC");
        }
        
        // Verificar bridge
        if (useBridge) {
            checkBridgeInterface();
        }
        
        // Buscar discos e ISOs
        diskFiles = findAllDisks();
        isoFiles = findAllISOs();
        
        if (diskFiles.empty()) {
            warning("No disk images found!");
            if (createDefaultDisk()) {
                diskFiles = findAllDisks();
            }
        }
        
        if (diskFiles.empty() && isoFiles.empty()) {
            error("No bootable media available!");
            error("Please add disk images to ./devices/disk/ or ISOs to ./devices/rom/");
            return false;
        }
        
        std::cout << "\n";
        printConfiguration();
        
        // Determinar modo de arranque
        if (!isoFiles.empty() && !diskFiles.empty()) {
            log("Boot Mode: ISO Installation with persistent disk(s)");
        } else if (!isoFiles.empty()) {
            log("Boot Mode: Live ISO (no persistent storage)");
        } else {
            log("Boot Mode: Disk boot");
        }
        
        std::cout << "\n";
        log("Starting virtual machine...");
        std::cout << "\n";
        
        if (!startQEMU()) {
            error("Failed to start QEMU!");
            return false;
        }
        
        success("QEMU started successfully!");
        
        if (useVNC) {
            if (!startWebsockify()) {
                cleanup();
                return false;
            }
            
            success("Websockify started successfully!");
            std::cout << "\n";
            std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
            std::cout << "║  VM is ready! Access via web browser:                         ║\n";
            std::cout << "║                                                               ║\n";
            std::cout << "║  🌐 http://localhost:8080/vnc.html?resize=remote&autoconnect=true  ║\n";
            std::cout << "║                                                               ║\n";
            std::cout << "║  Features: Remote scaling, auto-connect, full control         ║\n";
            std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";
        } else {
            success("VM started in local display mode!");
        }
        
        std::cout << "\n";
        log("Press Ctrl+C to shutdown the VM");
        std::cout << "\n";
        
        return true;
    }

    void setVNCMode(bool enabled) { useVNC = enabled; }
    void setBridgeMode(bool enabled) { useBridge = enabled; }
    void setCPUCores(int cores) { cpuCores = cores; }
    void setRAM(int gb) { ramGB = gb; }
    void setCamera(bool enabled) { enableCamera = enabled; }
    void setMicrophone(bool enabled) { enableMicrophone = enabled; }
};

// Variable global para cleanup
ColdVM* globalVM = nullptr;

void signalHandler(int sig) {
    std::cout << "\n";
    if (globalVM) {
        globalVM->cleanup();
    }
    std::cout << "\n✓ Cold VM shutdown complete!\n" << std::endl;
    exit(0);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    ColdVM vm;
    globalVM = &vm;
    
    // Procesar argumentos
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-vnc") {
            vm.setVNCMode(false);
        } else if (arg == "--no-bridge") {
            vm.setBridgeMode(false);
        } else if (arg == "--no-camera") {
            vm.setCamera(false);
        } else if (arg == "--no-mic") {
            vm.setMicrophone(false);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Cold VM Manager - Advanced Virtual Machine System\n\n";
            std::cout << "Usage: " << argv[0] << " [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --no-vnc      Use local GTK display instead of VNC\n";
            std::cout << "  --no-bridge   Use NAT networking instead of bridge\n";
            std::cout << "  --no-camera   Disable camera passthrough\n";
            std::cout << "  --no-mic      Disable microphone\n";
            std::cout << "  --help, -h    Show this help message\n\n";
            std::cout << "Default configuration:\n";
            std::cout << "  - 6 GB RAM\n";
            std::cout << "  - 4 CPU cores (host model)\n";
            std::cout << "  - VirtIO devices\n";
            std::cout << "  - VNC with remote scaling\n";
            std::cout << "  - Bridge networking (virbr0)\n";
            std::cout << "  - Camera, audio & microphone enabled\n";
            std::cout << "\n";
            return 0;
        }
    }
    
    if (vm.boot()) {
        while (true) {
            sleep(1);
        }
    } else {
        std::cerr << "\n✗ Failed to start Cold VM!\n" << std::endl;
        return 1;
    }
    
    return 0;
}
