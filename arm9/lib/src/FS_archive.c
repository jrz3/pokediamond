#include "FS_archive.h"
#include "FS_file.h"
#include "FS_command.h"
#include "FSi_util.h"
#include "MI_memory.h"
#include "MI_byteAccess.h"
#include "OS_printf.h"

FSArchive * arc_list = NULL;
FSDirPos current_dir_pos;

ARM_FUNC u32 FSi_GetPackedName(const char * name, int name_len)
{
    u32 ret = 0;
    if (name_len <= FS_ARCHIVE_NAME_LEN_MAX)
    {
        int i = 0;
        for (; i < name_len; i++)
        {
            u32 c = MI_ReadByte(name + i);
            if (!c)
                break;
            c = (u32)(c - 'A');
            if (c <= (u32)('Z' - 'A'))
                c = (u32)(c + 'a');
            else
                c = (u32)(c + 'A');
            ret |= (u32)(c << (i * 8));
        }
    }
    return ret;
}

ARM_FUNC FSResult FSi_ReadMemCallback(struct FSArchive * p_arc, void * dest, u32 pos, u32 size)
{
    MI_CpuCopy8((const void *)FS_GetArchiveOffset(p_arc, pos), dest, size);
    return FS_RESULT_SUCCESS;
}

ARM_FUNC FSResult FSi_WriteMemCallback(struct FSArchive * p_arc, const void * src, u32 pos, u32 size)
{
    MI_CpuCopy8(src, (void *)FS_GetArchiveOffset(p_arc, pos), size);
    return FS_RESULT_SUCCESS;
}

ARM_FUNC FSResult FSi_ReadMemoryCore(FSArchive * p_arc, void * dest, u32 pos, u32 size)
{
    MI_CpuCopy8((const void *)pos, dest, size);
    return FS_RESULT_SUCCESS;
}

FSFile * FSi_NextCommand(FSArchive * p_arc)
{
    OSIntrMode bak_psr = OS_DisableInterrupts();
    if (FSi_IsArchiveCanceling(p_arc))
    {
        FSFile *p, *q;
        p_arc->flag &= ~FS_ARCHIVE_FLAG_CANCELING;
        for (p = p_arc->list.next; p; p = q)
        {
            q = p->link.next;
            if (FS_IsCanceling(p))
            {
                if (p_arc->list.next == p)
                    p_arc->list.next = q;
                FSi_ReleaseCommand(p, FS_RESULT_CANCELED);
                if (!q)
                    q = p_arc->list.next;
            }
        }
    }
    if (!FSi_IsArchiveSuspending(p_arc) && !FS_IsArchiveSuspended(p_arc))
    {
        FSFile * p_file = p_arc->list.next;
        if (p_file != NULL)
        {
            const BOOL is_start = !FSi_IsArchiveRunning(p_arc);
            if (is_start)
                p_arc->flag |= FS_ARCHIVE_FLAG_RUNNING;
            OS_RestoreInterrupts(bak_psr);
            if (is_start)
            {
                if ((p_arc->proc_flag & FS_ARCHIVE_PROC_ACTIVATE) != 0)
                    (*p_arc->proc) (p_file, FS_COMMAND_ACTIVATE);
            }
            bak_psr = OS_DisableInterrupts();
            p_file->stat |= FS_FILE_STATUS_OPERATING;
            if (FS_IsFileSyncMode(p_file))
            {
                OS_WakeupThread(p_file->queue);
                OS_RestoreInterrupts(bak_psr);
                return NULL;
            }
            OS_RestoreInterrupts(bak_psr);
            return p_file;
        }
    }
    if (FSi_IsArchiveRunning(p_arc))
    {
        p_arc->flag &= ~FS_ARCHIVE_FLAG_RUNNING;
        if (p_arc->proc_flag & FS_ARCHIVE_PROC_IDLE)
        {
            FSFile tmp;
            FS_InitFile(&tmp);
            tmp.arc = p_arc;
            (*p_arc->proc)(&tmp, FS_COMMAND_IDLE);
        }
    }
    if (FSi_IsArchiveSuspending(p_arc))
    {
        p_arc->flag &= ~FS_ARCHIVE_FLAG_SUSPENDING;
        p_arc->flag |= FS_ARCHIVE_FLAG_SUSPEND;
        OS_WakeupThread(&p_arc->stat_q);
    }
    OS_RestoreInterrupts(bak_psr);
    return NULL;
}

ARM_FUNC void FSi_ExecuteAsyncCommand(FSFile * p_file)
{
    FSArchive *const p_arc = p_file->arc;
    while (p_file)
    {
        OSIntrMode bak_psr = OS_DisableInterrupts();
        p_file->stat |= FS_FILE_STATUS_OPERATING;
        if (FS_IsFileSyncMode(p_file))
        {
            OS_WakeupThread(p_file->queue);
            OS_RestoreInterrupts(bak_psr);
            break;
        }
        p_file->stat |= FS_FILE_STATUS_ASYNC;
        OS_RestoreInterrupts(bak_psr);
        if (FSi_TranslateCommand(p_file, p_file->command) == FS_RESULT_PROC_ASYNC)
            break;
        p_file = FSi_NextCommand(p_arc);
    }
}

ARM_FUNC BOOL FSi_ExecuteSyncCommand(FSFile * p_file)
{
    FSFile * p_target;
    FSResult ret = FSi_TranslateCommand(p_file, p_file->command);
    FSi_ReleaseCommand(p_file, ret);
    p_target = FSi_NextCommand(p_file->arc);
    if (p_target)
        FSi_ExecuteAsyncCommand(p_target);
    return FS_IsSucceeded(p_file);
}

ARM_FUNC BOOL FSi_SendCommand(FSFile * p_file, FSCommandType command)
{
    FSArchive * p_arc = p_file->arc;
    const int bit = 1 << command;
    p_file->command = command;
    p_file->error = FS_RESULT_BUSY;
    p_file->stat |= FS_FILE_STATUS_BUSY;
    {
        OSIntrMode bak_psr = OS_DisableInterrupts();
        if (FSi_IsArchiveUnloading(p_arc))
        {
            FSi_ReleaseCommand(p_file, FS_RESULT_CANCELLED);
            OS_RestoreInterrupts(bak_psr);
            return FALSE;
        }
        if ((bit & FS_ARCHIVE_PROC_SYNC) != 0)
            p_file->stat |= FS_FILE_STATUS_SYNC;
        FSi_AppendToList(p_file, (FSFile *)&p_arc->list);
        if (!FS_IsArchiveSuspended(p_arc) && !FSi_IsArchiveRunning(p_arc))
        {
            p_arc->flag |= FS_ARCHIVE_FLAG_RUNNING;
            OS_RestoreInterrupts(bak_psr);
            if ((p_arc->proc_flag & FS_ARCHIVE_PROC_ACTIVATE))
                (*p_arc->proc)(p_file, FS_COMMAND_ACTIVATE);
            bak_psr = OS_DisableInterrupts();
            p_file->stat |= FS_FILE_STATUS_OPERATING;
            if (!FS_IsFileSyncMode(p_file))
            {
                OS_RestoreInterrupts(bak_psr);
                FSi_ExecuteAsyncCommand(p_file);
                return TRUE;
            }
            OS_RestoreInterrupts(bak_psr);
        }
        else if (!FS_IsFileSyncMode(p_file))
        {
            OS_RestoreInterrupts(bak_psr);
            return TRUE;
        }
        else
        {
            do
            {
                OS_SleepThread(p_file->queue);
            } while (!(p_file->stat & FS_FILE_STATUS_OPERATING));
            OS_RestoreInterrupts(bak_psr);
        }
    }
    return FSi_ExecuteSyncCommand(p_file);
}

ARM_FUNC void FS_InitArchive(FSArchive * p_arc)
{
    MI_CpuClear8(p_arc, sizeof(FSArchive));
    p_arc->sync_q.head = p_arc->sync_q.tail = NULL;
    p_arc->stat_q.head = p_arc->stat_q.tail = NULL;
}

FSArchive * const FS_FindArchive(const char * name, int name_len)
{
    u32 pack = FSi_GetPackedName(name, name_len);
    OSIntrMode bak_psr = OS_DisableInterrupts();
    FSArchive * p_arc = arc_list;
    while (p_arc && (p_arc->name.pack != pack))
        p_arc = p_arc->next;
    OS_RestoreInterrupts(bak_psr);
    return p_arc;
}

ARM_FUNC BOOL FS_RegisterArchiveName(FSArchive * p_arc, const char * name, int name_len)
{
    BOOL ret = FALSE;
    OSIntrMode bak_psr = OS_DisableInterrupts();
    if (!FS_FindArchive(name, name_len))
    {
        FSArchive * p_tail = arc_list;
        if (!p_tail)
        {
            arc_list = p_arc;
            current_dir_pos.arc = p_arc;
            current_dir_pos.pos = 0;
            current_dir_pos.index = 0;
            current_dir_pos.own_id = 0;
        }
        else
        {
            while (p_tail->next)
                p_tail = p_tail->next;
            p_tail->next = p_arc;
            p_arc->prev = p_tail;
        }
        p_arc->name.pack = FSi_GetPackedName(name, name_len);
        p_arc->flag |= FS_ARCHIVE_FLAG_REGISTER;
        ret = TRUE;
    }
    OS_RestoreInterrupts(bak_psr);
    return ret;
}

ARM_FUNC void FS_ReleaseArchiveName(FSArchive * p_arc)
{
    if (p_arc->name.pack)
    {
        OSIntrMode bak_psr = OS_DisableInterrupts();
        if (p_arc->next)
            p_arc->next->prev = p_arc->prev;
        if (p_arc->prev)
            p_arc->prev->next = p_arc->next;
        p_arc->name.pack = 0;
        p_arc->next = p_arc->prev = NULL;
        p_arc->flag &= ~FS_ARCHIVE_FLAG_REGISTER;
        if (current_dir_pos.arc == p_arc)
        {
            current_dir_pos.arc = arc_list;
            current_dir_pos.pos = 0;
            current_dir_pos.index = 0;
            current_dir_pos.own_id = 0;
        }
        OS_RestoreInterrupts(bak_psr);
    }
}

ARM_FUNC BOOL FS_LoadArchive(FSArchive * p_arc, u32 base, u32 fat, u32 fat_size, u32 fnt, u32 fnt_size, FS_ARCHIVE_READ_FUNC read_func, FS_ARCHIVE_WRITE_FUNC write_func)
{
    p_arc->base = base;
    p_arc->fat_size = fat_size;
    p_arc->fat = p_arc->fat_bak = fat;
    p_arc->fnt_size = fnt_size;
    p_arc->fnt = p_arc->fnt_bak = fnt;
    p_arc->read_func = (read_func != NULL) ? read_func : FSi_ReadMemCallback;
    p_arc->write_func = (write_func != NULL) ? write_func : FSi_WriteMemCallback;
    p_arc->table_func = p_arc->read_func;
    p_arc->load_mem = NULL;
    p_arc->flag |= FS_ARCHIVE_FLAG_LOADED;
    return TRUE;
}

ARM_FUNC BOOL FS_UnloadArchive(FSArchive * p_arc)
{
    OSIntrMode bak_psr = OS_DisableInterrupts();
    if (FS_IsArchiveLoaded(p_arc))
    {
        if (FS_IsArchiveTableLoaded(p_arc))
        {
            OS_TWarning("memory may leak. preloaded-table of archive \"%s\" (0x%08X)", p_arc->name.ptr, p_arc->load_mem);
        }
        {
            FSFile *p, *q;
            BOOL bak_state = FS_SuspendArchive(p_arc);
            p_arc->flag |= FS_ARCHIVE_FLAG_UNLOADING;
            for (p = p_arc->list.next; p; p = q)
            {
                q = p->link.next;
                FSi_ReleaseCommand(p, FS_RESULT_CANCELED);
            }
            p_arc->list.next = NULL;
            if (bak_state)
                FS_ResumeArchive(p_arc);
        }
        p_arc->base = 0;
        p_arc->fat = 0;
        p_arc->fat_size = 0;
        p_arc->fnt = 0;
        p_arc->fnt_size = 0;
        p_arc->fat_bak = p_arc->fnt_bak = 0;
        p_arc->flag &= ~(FS_ARCHIVE_FLAG_CANCELING | FS_ARCHIVE_FLAG_LOADED | FS_ARCHIVE_FLAG_UNLOADING);
    }
    OS_RestoreInterrupts(bak_psr);
    return TRUE;
}

ARM_FUNC u32 FS_LoadArchiveTables(FSArchive *p_arc, void *p_mem, u32 max_size)
{
    u32 total_size = ALIGN_BYTE(p_arc->fat_size + p_arc->fnt_size + 32, 32);
    if (total_size <= max_size)
    {
        u8     *p_cache = (u8 *)ALIGN_BYTE((u32)p_mem, 32);
        FSFile  tmp;
        FS_InitFile(&tmp);
        if (FS_OpenFileDirect(&tmp, p_arc, p_arc->fat, p_arc->fat + p_arc->fat_size, (u32)~0))
        {
            if (FS_ReadFile(&tmp, p_cache, (s32)p_arc->fat_size) < 0)
            {
                MI_CpuFill8(p_cache, 0x00, p_arc->fat_size);
            }
            FS_CloseFile(&tmp);
        }
        p_arc->fat = (u32)p_cache;
        p_cache += p_arc->fat_size;
        if (FS_OpenFileDirect(&tmp, p_arc, p_arc->fnt, p_arc->fnt + p_arc->fnt_size, (u32)~0))
        {
            if (FS_ReadFile(&tmp, p_cache, (s32)p_arc->fnt_size) < 0)
            {
                MI_CpuFill8(p_cache, 0x00, p_arc->fnt_size);
            }
            FS_CloseFile(&tmp);
        }
        p_arc->fnt = (u32)p_cache;
        p_arc->load_mem = p_mem;
        p_arc->table_func = FSi_ReadMemoryCore;
        p_arc->flag |= FS_ARCHIVE_FLAG_TABLE_LOAD;
    }
    return total_size;
}

void * FS_UnloadArchiveTables(FSArchive * p_arc)
{
    void *ret = NULL;
    if (FS_IsArchiveLoaded(p_arc))
    {
        BOOL bak_stat = FS_SuspendArchive(p_arc);
        if (FS_IsArchiveTableLoaded(p_arc))
        {
            p_arc->flag &= ~FS_ARCHIVE_FLAG_TABLE_LOAD;
            ret = p_arc->load_mem;
            p_arc->load_mem = NULL;
            p_arc->fat = p_arc->fat_bak;
            p_arc->fnt = p_arc->fnt_bak;
            p_arc->table_func = p_arc->read_func;
        }
        if (bak_stat)
            FS_ResumeArchive(p_arc);
    }
    return ret;
}

ARM_FUNC BOOL FS_SuspendArchive(FSArchive * p_arc)
{
    OSIntrMode bak_psr = OS_DisableInterrupts();
    const BOOL bak_stat = !FS_IsArchiveSuspended(p_arc);
    if (bak_stat)
    {
        if (FSi_IsArchiveRunning(p_arc))
        {
            p_arc->flag |= FS_ARCHIVE_FLAG_SUSPENDING;
            do {
                OS_SleepThread(&p_arc->stat_q);
            } while (FSi_IsArchiveSuspending(p_arc));
        }
        else
        {
            p_arc->flag |= FS_ARCHIVE_FLAG_SUSPEND;
        }
    }
    OS_RestoreInterrupts(bak_psr);
    return bak_stat;
}

ARM_FUNC BOOL FS_ResumeArchive(FSArchive * p_arc)
{
    FSFile * p_target = NULL;
    OSIntrMode bak_psr = OS_DisableInterrupts();
    const BOOL bak_stat = !FS_IsArchiveSuspended(p_arc);
    if (!bak_stat)
    {
        p_arc->flag &= ~FS_ARCHIVE_FLAG_SUSPEND;
        p_target = FSi_NextCommand(p_arc);
    }
    OS_RestoreInterrupts(bak_psr);
    if (p_target)
        FSi_ExecuteAsyncCommand(p_target);
    return bak_stat;
}

ARM_FUNC void FS_SetArchiveProc(struct FSArchive * p_arc, FS_ARCHIVE_PROC_FUNC proc, u32 flags)
{
    if (!flags)
        proc = NULL;
    else if (!proc)
        flags = 0;
    p_arc->proc = proc;
    p_arc->proc_flag = flags;
}

ARM_FUNC void FS_NotifyArchiveAsyncEnd(FSArchive *p_arc, FSResult ret)
{
    if (FSi_IsArchiveAsync(p_arc))
    {
        FSFile *p_file = p_arc->list.next;
        p_arc->flag &= ~FS_ARCHIVE_FLAG_IS_ASYNC;
        FSi_ReleaseCommand(p_file, ret);
        p_file = FSi_NextCommand(p_arc);
        if (p_file)
            FSi_ExecuteAsyncCommand(p_file);
    }
    else
    {
        FSFile *p_file = p_arc->list.next;
        OSIntrMode bak_psr = OS_DisableInterrupts();
        p_file->error = ret;
        p_arc->flag &= ~FS_ARCHIVE_FLAG_IS_SYNC;
        OS_WakeupThread(&p_arc->sync_q);
        (void)OS_RestoreInterrupts(bak_psr);
    }
}
