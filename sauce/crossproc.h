//
//  crossproc.h
//  h5gg
//
//  Created by admin on 25/4/2022.
//

#ifndef crossproc_h
#define crossproc_h


#import <sys/sysctl.h>
#import <mach-o/dyld_images.h>

extern "C" {
#include "dyld64.h"
#include "libproc.h"
#include "proc_info.h"
}

NSArray* getRunningProcess()
{
    //指定名字参数，按照顺序第一个元素指定本请求定向到内核的哪个子系统，第二个及其后元素依次细化指定该系统的某个部分。
    //CTL_KERN，KERN_PROC,KERN_PROC_ALL 正在运行的所有进程
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL ,0};
    
    u_int miblen = 4;
    //值-结果参数：函数被调用时，size指向的值指定该缓冲区的大小；函数返回时，该值给出内核存放在该缓冲区中的数据量
    //如果这个缓冲不够大，函数就返回ENOMEM错误
    size_t size;
    //返回0，成功；返回-1，失败
    int st = sysctl(mib, miblen, NULL, &size, NULL, 0);
    NSLog(@"allproc=%d, %s", st, strerror(errno));
    
    struct kinfo_proc * process = NULL;
    struct kinfo_proc * newprocess = NULL;
    do
    {
        size += size / 10;
        newprocess = (struct kinfo_proc *)realloc(process, size);
        if (!newprocess)
        {
            if (process)
            {
                free(process);
                process = NULL;
            }
            return nil;
        }
        
        process = newprocess;
        st = sysctl(mib, miblen, process, &size, NULL, 0);
        NSLog(@"allproc=%d, %s", st, strerror(errno));
    } while (st == -1 && errno == ENOMEM);
    
    if (st == 0)
    {
        if (size % sizeof(struct kinfo_proc) == 0)
        {
            int nprocess = size / sizeof(struct kinfo_proc);
            if (nprocess)
            {
                NSMutableArray * array = [[NSMutableArray alloc] init];
                for (int i = nprocess - 1; i >= 0; i--)
                {
                    [array addObject:@{
                        @"pid": [NSNumber numberWithInt:process[i].kp_proc.p_pid],
                        @"name": [NSString stringWithUTF8String:process[i].kp_proc.p_comm]
                    }];
                }
                
                free(process);
                process = NULL;
                NSLog(@"allproc=%d, %@", array.count, array);
                return array;
            }
        }
    }
    
    return nil;
}

pid_t pid_for_name(const char* name)
{
    NSArray* allproc = getRunningProcess();
    for(NSDictionary* proc in allproc)
    {
        if([[proc valueForKey:@"name"] isEqualToString:[NSString stringWithUTF8String:name]])
            return [[proc valueForKey:@"pid"] intValue];
    }
    return 0;
}

size_t getMachoVMSize(task_port_t task, mach_vm_address_t addr)
{
    struct mach_header_64 header;
    mach_vm_size_t hdrsize = sizeof(header);
    kern_return_t kr = mach_vm_read_overwrite(task, addr, hdrsize, (mach_vm_address_t)&header, &hdrsize);
    if(kr != KERN_SUCCESS)
        return 0;
    
    size_t sz = sizeof(header); // Size of the header
    sz += header.sizeofcmds;    // Size of the load commands
    
    mach_vm_size_t lcsize=header.sizeofcmds;
    void* buf = malloc(lcsize);
    
    kr = mach_vm_read_overwrite(task, addr+hdrsize, lcsize, (mach_vm_address_t)buf, &lcsize);
    if(kr == KERN_SUCCESS)
    {
        struct load_command* lc = (struct load_command*)buf;
        for (uint32_t i = 0; i < header.ncmds; i++) {
            if (lc->cmd == LC_SEGMENT_64) {
                struct segment_command_64 * sc = (struct segment_command_64 *) lc;
                if(!(sc->vmaddr==0 && sc->vmsize==0x100000000 && sc->fileoff==0 && sc->filesize==0 && sc->flags==0 && sc->initprot==0 && sc->maxprot==0)) //skip __PAGEZERO
                sz += ((struct segment_command_64 *) lc)->vmsize; // Size of segments
            }
            lc = (struct load_command *) ((char *)lc + lc->cmdsize);
        }
    }
    free(buf);
    return sz;
}

NSArray* getRangesList2(task_port_t task, NSString* filter)
{
    NSMutableArray* results = [[NSMutableArray alloc] init];
    
    task_dyld_info_data_t task_dyld_info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t kr = task_info(task, TASK_DYLD_INFO, (task_info_t)&task_dyld_info, &count);
    NSLog(@"getmodules TASK_DYLD_INFO=%p", task_dyld_info.all_image_info_addr);
    
    if(kr!=KERN_SUCCESS)
        return results;
    
    struct dyld_all_image_infos64 aii;
    mach_vm_size_t aiiSize = sizeof(aii);
    kr = mach_vm_read_overwrite(task, task_dyld_info.all_image_info_addr, aiiSize, (mach_vm_address_t)&aii, &aiiSize);
    
    NSLog(@"getmodules all_image_info %d %p %d", aii.version, aii.infoArray, aii.infoArrayCount);
    if(kr != KERN_SUCCESS)
        return results;
    
    mach_vm_address_t        ii;
    uint32_t                iiCount;
    mach_msg_type_number_t    iiSize;
    
    
    ii = aii.infoArray;
    iiCount = aii.infoArrayCount;
    iiSize = iiCount * sizeof(struct dyld_image_info64);
        
    // If ii is NULL, it means it is being modified, come back later.
    kr = mach_vm_read(task, ii, iiSize, (vm_offset_t *)&ii, &iiSize);
    if(kr != KERN_SUCCESS) {
        NSLog(@"getmodules cannot read aii");
        return results;
    }
    
    for (int i = 0; i < iiCount; i++) {
        mach_vm_address_t addr;
        mach_vm_address_t path;
        
        struct dyld_image_info64 *ii64 = (struct dyld_image_info64 *)ii;
        addr = ii64[i].imageLoadAddress;
        path = ii64[i].imageFilePath;
        
        NSLog(@"getmodules image[%d] %p %p", i, addr, path);
        
        char pathbuffer[PATH_MAX];
        
        mach_vm_size_t size3;
        if (mach_vm_read_overwrite(task, path, MAXPATHLEN, (mach_vm_address_t)pathbuffer, &size3) != KERN_SUCCESS)
            strcpy(pathbuffer, "<Unknown>");
        
        NSLog(@"getmodules path=%s", pathbuffer);
        
        if(filter==nil
            || (i==0 && [filter isEqual:@"0"])
            || [filter isEqual:[NSString stringWithUTF8String:basename((char*)pathbuffer) ]]
        ){
            [results addObject:@{
                @"name" : [NSString stringWithUTF8String:pathbuffer],
                @"start" : [NSString stringWithFormat:@"0x%llX", addr],
                @"end" : [NSString stringWithFormat:@"0x%llX",
                          (uint64_t)addr+getMachoVMSize(task, (uint64_t)addr) ],
                //@"type" : @"rwxp",
            }];
            
            if(i==0 && [filter isEqual:@"0"]) break;
        }
    }
    vm_deallocate(mach_task_self(), ii, iiSize);

    return results;
}

#endif /* crossproc_h */
